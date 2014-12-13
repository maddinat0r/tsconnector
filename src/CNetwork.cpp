#include "main.h"
#include "CNetwork.h"
#include "CServer.h"
#include "CUtils.h"
#include "CCallback.h"

#include <istream>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "format.h"


void CNetwork::NetAlive(const boost::system::error_code &error_code, bool from_write)
{
	if (from_write == true)
		return;
	

	if (m_Socket.is_open())
	{
		static string empty_data("\n");
		m_Socket.async_send(asio::buffer(empty_data), 
			boost::bind(&CNetwork::NetAlive, this, boost::asio::placeholders::error, true));
	}
	m_AliveTimer.expires_from_now(boost::posix_time::seconds(60));
	m_AliveTimer.async_wait(
		boost::bind(&CNetwork::NetAlive, this, boost::asio::placeholders::error, false));
}


bool CNetwork::Connect(string hostname, unsigned short port, unsigned short query_port)
{
	boost::system::error_code error;
	tcp::resolver resolver(m_IoService);
	tcp::resolver::query query(tcp::v4(), hostname, string());
	tcp::resolver::iterator i = resolver.resolve(query, error);

	if (error)
	{
		CCallbackHandler::Get()->ForwardError(
			EErrorType::CONNECTION_ERROR, error.value(),
			fmt::format("error while resolving hostname \"{}\": {}", hostname, error.message()));
		return false;
	}


	m_SocketDest = *i;
	m_SocketDest.port(query_port);
	m_ServerPort = port;

	AsyncConnect();
	m_IoThread = new thread(boost::bind(&asio::io_service::run, boost::ref(m_IoService)));

	return true;
}

bool CNetwork::Disconnect()
{
	if (m_Socket.is_open() == false || m_Connected == false)
		return false; 


	m_Connected = false;
	m_Socket.close();
	m_IoService.stop();
	
	if (m_IoThread != nullptr)
	{
		if (m_IoThread->get_id() != boost::this_thread::get_id())
			m_IoThread->join();
		delete m_IoThread;
		m_IoThread = nullptr;
	}
	return true;
}

void CNetwork::AsyncRead()
{
	asio::async_read_until(m_Socket, m_ReadStreamBuf, '\r', 
		boost::bind(&CNetwork::OnRead, this, _1));
}

void CNetwork::AsyncWrite(string &data)
{
	m_CmdWriteBuffer = data;
	if (data.at(data.length()-1) != '\n')
		m_CmdWriteBuffer.push_back('\n');

	m_Socket.async_send(asio::buffer(m_CmdWriteBuffer), 
		boost::bind(&CNetwork::OnWrite, this, _1));
}

void CNetwork::AsyncConnect()
{
	if (m_Socket.is_open())
		m_Socket.close();

	m_Connected = false;
	m_Socket.async_connect(m_SocketDest, boost::bind(&CNetwork::OnConnect, this, _1));
}

void CNetwork::OnConnect(const boost::system::error_code &error_code)
{
	if (error_code.value() == 0)
	{
		m_Connected = true;
		Execute(fmt::format("use port={}", m_ServerPort));
		AsyncRead();
	}
	else
	{
		CCallbackHandler::Get()->ForwardError(
			EErrorType::CONNECTION_ERROR, error_code.value(),
			fmt::format("error while connecting to server: {}", error_code.message()));
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

#ifdef _DEBUG
		if (read_data.length() < 512)
			logprintf(">>>> %s", read_data.c_str());
		else
		{
			string shortened_data(read_data);
			shortened_data.resize(256);
			logprintf(">>>> %s", shortened_data.c_str());
		}
#endif

		//regex: parse error
		//if this is an error message, it means that no other result data will come
		static const boost::regex error_rx("error id=([0-9]+) msg=([^ \n]+)");
		boost::smatch error_rx_result;
		if (boost::regex_search(read_data, error_rx_result, error_rx))
		{
			if (error_rx_result[1].str() == "0")
			{
				for (auto i = captured_data.begin(); i != captured_data.end(); ++i)
				{
					string &data = *i;
					if (data.find('|') == string::npos) 
						continue;

					//we have multiple data rows with '|' as delimiter here,
					//split them up and re-insert every single row
					vector<string> result_set;
					size_t delim_pos = 0;
					do
					{
						size_t old_delim_pos = delim_pos;
						delim_pos = data.find('|', delim_pos);
						string row = data.substr(old_delim_pos, delim_pos - old_delim_pos);
						result_set.push_back(row);
					} while (delim_pos != string::npos && ++delim_pos);

					i = captured_data.erase(i);
					for (auto j = result_set.begin(), jend = result_set.end(); j != jend; ++j)
						i = captured_data.insert(i, *j);
				}
				
				//call callback and send next command
				m_CmdQueueMutex.lock();
				if (m_CmdQueue.empty() == false)
				{
					ReadCallback_t &callback = m_CmdQueue.front().get<1>();
					m_CmdQueueMutex.unlock();
					if (callback)
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
				unsigned int error_id = 0;

				CUtils::Get()->UnEscapeString(error_str);
				CUtils::Get()->ConvertStringToInt(error_rx_result[1].str(), error_id);

				CCallbackHandler::Get()->ForwardError(
					EErrorType::TEAMSPEAK_ERROR, error_id,
					fmt::format("error while executing \"{}\": {}", m_CmdQueue.front().get<0>(), error_str));
				m_CmdQueue.pop();
			}

			captured_data.clear();
		}
		else if (read_data.find("notify") == 0)
		{
			//check if notify is duplicate
			static string last_notify_data;
			static const vector<string> duplicate_notifies{ 
				"notifyclientmoved", 
				"notifycliententerview", 
				"notifyclientleftview" 
			};
			bool is_duplicate = false;
			
			for (auto &s : duplicate_notifies)
			{
				if (read_data.find(s) == 0)
				{
					if (last_notify_data == read_data)
						is_duplicate = true;
					
					break;
				}
			}
			
			if (is_duplicate == false)
			{
				//notify event
				boost::smatch event_result;
				for (auto &event : m_EventList)
				{
					if (boost::regex_search(read_data, event_result, event.get<0>()))
					{
						event.get<1>()(event_result);
						break;
					}
				}
			}

			last_notify_data = read_data;
		}
		else
		{
			//stack the result data if it is not an error or notification message
			captured_data.push_back(read_data);
		}

		AsyncRead();
	}
	else if (error_code == asio::error::eof)
	{
		AsyncConnect();
	}
	else
	{
		CCallbackHandler::Get()->ForwardError(
			EErrorType::CONNECTION_ERROR, error_code.value(),
			fmt::format("error while reading: {}", error_code.message()));
	}
}

void CNetwork::OnWrite(const boost::system::error_code &error_code)
{
#ifdef _DEBUG
	logprintf("<<<< %s", m_CmdWriteBuffer.c_str());
#endif
	m_CmdWriteBuffer.clear();
	if (error_code.value() != 0)
	{
		CCallbackHandler::Get()->ForwardError(
			EErrorType::CONNECTION_ERROR, error_code.value(),
			fmt::format("error while writing: {}", error_code.message()));
	}
}

void CNetwork::Execute(string cmd, ReadCallback_t callback)
{
	boost::lock_guard<boost::mutex> queue_lock_guard(m_CmdQueueMutex);
	m_CmdQueue.push(boost::make_tuple(boost::move(cmd), boost::move(callback)));
	if (m_CmdQueue.size() == 1)
		AsyncWrite(m_CmdQueue.front().get<0>());
}
