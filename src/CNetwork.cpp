#include "main.h"
#include "CNetwork.h"
#include "CServer.h"
#include "CUtils.h"

#include <istream>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "format.h"


void CNetwork::NetAlive(const boost::system::error_code &error_code, bool from_write)
{
	if (from_write == false)
	{
		if (m_Socket.is_open())
		{
			static string empty_data("\n");
			m_Socket.async_send(asio::buffer(empty_data), boost::bind(&CNetwork::NetAlive, this, boost::asio::placeholders::error, true));
		}
		m_AliveTimer.expires_from_now(boost::posix_time::seconds(60));
		m_AliveTimer.async_wait(boost::bind(&CNetwork::NetAlive, this, boost::asio::placeholders::error, false));
	}
}


void CNetwork::Connect(char *ip, unsigned short port, unsigned short query_port)
{
	m_SocketDest = tcp::endpoint(asio::ip::address::from_string(ip), query_port);
	m_ServerPort = port;
	AsyncConnect();
	m_IoThread = new thread(boost::bind(&asio::io_service::run, boost::ref(m_IoService)));

	Execute(str(fmt::Writer() << "use port=" << m_ServerPort));
}

void CNetwork::Disconnect()
{
	m_Socket.close();
	m_IoService.stop();
	
	m_IoThread->join();
	delete m_IoThread;
}

void CNetwork::AsyncRead()
{
	asio::async_read_until(m_Socket, m_ReadStreamBuf, '\r', boost::bind(&CNetwork::OnRead, this, _1));
}

void CNetwork::AsyncWrite(string &data)
{
	m_CmdWriteBuffer = data;
	if (data.at(data.length()-1) != '\n')
		m_CmdWriteBuffer.push_back('\n');

	m_Socket.async_send(asio::buffer(m_CmdWriteBuffer), boost::bind(&CNetwork::OnWrite, this, _1));
}

void CNetwork::AsyncConnect()
{
	if (m_Socket.is_open())
		m_Socket.close();

	m_Socket.async_connect(m_SocketDest, boost::bind(&CNetwork::OnConnect, this, _1));
}

void CNetwork::OnConnect(const boost::system::error_code &error_code)
{
	if (error_code.value() == 0)
	{
		AsyncRead();
	}
	else
	{
		logprintf(">> plugin.TSConnector: Error while connecting to server: \"%s\"", error_code.message().c_str());
		Disconnect();
	}
}


/*
	- result data is sent as a string which ends with "\n\r"
	- the Teamspeak3 server can send multiple strings
	- the end of a result set is always an error result string
*/
void CNetwork::OnRead(const boost::system::error_code &error_code)
{
	if (error_code.value() == 0)
	{
		static vector<string> captured_data;
		std::istream tmp_stream(&m_ReadStreamBuf);
		string read_data;
		std::getline(tmp_stream, read_data, '\r');


		//regex: parse error
		//if this is an error message, it means that no other result data will come
		static const boost::regex error_rx("error id=([0-9]+) msg=([^ \n]+)");
		boost::smatch error_rx_result;
		if (boost::regex_search(read_data, error_rx_result, error_rx))
		{
			if (error_rx_result[1].str() == "0")
			{
				for (vector<string>::iterator i = captured_data.begin(); i != captured_data.end(); ++i)
				{
					string &data = *i;
					if (data.find('|') != string::npos) //multiple data rows with '|' as delimiter
					{
						vector<string> result_set;
						size_t delim_pos = 0;
						do
						{
							size_t old_delim_pos = delim_pos;
							delim_pos = data.find('|', delim_pos);
							string row = data.substr(old_delim_pos, delim_pos - old_delim_pos);
							result_set.push_back(row);
						} while (delim_pos != string::npos && ++delim_pos);

						i = captured_data.insert(captured_data.erase(i), result_set.begin(), result_set.end());
					}
				}
				
				//call callback and send next command
				m_CmdQueueMutex.lock();
				if (m_CmdQueue.empty() == false)
				{
					ReadCallback_t &callback = m_CmdQueue.front().get<1>();
					m_CmdQueueMutex.unlock();
					if (callback.empty() == false)
						callback(captured_data); //calls the callback
					m_CmdQueueMutex.lock();
					m_CmdQueue.pop();

					if (m_CmdQueue.empty() == false)
						AsyncWrite(m_CmdQueue.front().get<0>());
				}
				m_CmdQueueMutex.unlock();
			}
			else
			{
				string error_str(error_rx_result[2].str());
				CUtils::Get()->UnEscapeString(error_str);

				logprintf(">> plugin.TSConnector: Error while executing \"%s\": %s (#%s)", m_CmdQueue.front().get<0>().c_str(), error_str.c_str(), error_rx_result[1].str().c_str());
				m_CmdQueue.pop();
			}

			captured_data.clear();
		}
		else if (read_data.find("notify") == 0)
		{
			static string last_notify_data;
			//check if notify is duplicate
			if (last_notify_data != read_data)
			{
				//notify event
				boost::smatch event_result;
				for (vector<EventTuple_t>::iterator i = m_EventList.begin(); i != m_EventList.end(); ++i)
				{
					if (boost::regex_search(read_data, event_result, i->get<0>()))
					{
						i->get<1>()(event_result);
						break;
					}
				}
			}
			last_notify_data = read_data;
		}
		else
		{
			//stack the result data if it is not an error message
			captured_data.push_back(read_data);
		}

		AsyncRead();
	}
	else if (error_code == asio::error::eof)
	{
		AsyncConnect();
	}
	else
		logprintf(">> plugin.TSConnector: Error while reading: %s (#%d)", error_code.message().c_str(), error_code.value());
}

void CNetwork::OnWrite(const boost::system::error_code &error_code)
{
	m_CmdWriteBuffer.clear();
	if (error_code.value() != 0)
		logprintf(">> plugin.TSConnector: Error while writing: %s (#%d)", error_code.message().c_str(), error_code.value());
}

void CNetwork::Execute(string cmd, ReadCallback_t callback)
{
	boost::lock_guard<boost::mutex> queue_lock_guard(m_CmdQueueMutex);
	m_CmdQueue.push(boost::make_tuple(boost::move(cmd), boost::move(callback)));
	if (m_CmdQueue.size() == 1)
		AsyncWrite(m_CmdQueue.front().get<0>());
}

void CNetwork::RegisterEvent(boost::regex &event_rx, EventCallback_t callback)
{
	m_EventList.push_back(boost::make_tuple(event_rx, boost::move(callback)));
}
