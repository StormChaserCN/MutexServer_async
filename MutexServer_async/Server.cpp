#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>  //mutex
#include <boost/typeof/typeof.hpp>

//STL
//#include <vector>
#include <unordered_map>
#include <iostream>

#define PORT 7781

#define RET_OK 0
#define RET_ERR -1

using namespace std;
using namespace boost;
using boost::asio::ip::tcp;

namespace basio = boost::asio;
unordered_map<string, unsigned long> iplist;
unordered_map<string, unsigned long> idwhitelist;
boost::mutex zmutex;


unsigned int ConvertIP(string strip)
{
	unsigned int nret = 0;
	vector<string> v;
	split(v, strip, is_any_of("."), token_compress_off);
	if (v.size() != 4)
	{
		return nret;
	}
	else
	{
		for (unsigned int i = 0; i < v.size(); ++i)
		{
			nret = nret * 256 + lexical_cast<unsigned short>(v[i]);
		}
	}
	return nret;
}

string ConvertIPToString(unsigned int ip)
{
	list<string> v;
	for (int i = 0; i < 3; ++i)
	{
		v.push_front(lexical_cast<string>(ip % 256));
		ip = ip / 256;
	}
	v.push_front(lexical_cast<string>(ip));
	return join(v, ".");
}

class session
{
public:
	session(basio::io_service& io_service)
		: socket_(io_service)
	{
	}

	tcp::socket& socket()
	{
		return socket_;
	}

	void start()
	{
		data_.fill(0);
		socket_.async_read_some(basio::buffer(data_),
			boost::bind(&session::handle_read, this,
			basio::placeholders::error,
			basio::placeholders::bytes_transferred));
	}

	void handle_read(const boost::system::error_code& ec,
		size_t bytes_transferred)
	{
		if (!ec)
		{
			// int nlen = HandleMSG(data_);
			int nlen = handle_msg();
			basio::async_write(socket_,
				basio::buffer(data_, nlen),
				boost::bind(&session::handle_write, this,
				basio::placeholders::error));
		}
		else
		{
			if (ec != basio::error::eof)
			{
				BOOST_LOG_TRIVIAL(error) << "Session:" << ec.message();
			}
			delete this;
		}
	}

	void handle_write(const boost::system::error_code& ec)
	{
		if (!ec)
		{
			socket_.async_read_some(basio::buffer(data_),
				boost::bind(&session::handle_read, this,
				basio::placeholders::error,
				basio::placeholders::bytes_transferred));
		}
		else
		{
			if (ec != basio::error::eof)
			{
				BOOST_LOG_TRIVIAL(error) << "Session:" << ec.message();
			}
			delete this;
		}
	}

	int handle_msg()
	{
		string msg(data_.c_array());
		data_.fill(0);
		trim(msg);
		vector<string> v;
		int nret = 0;
		split(v, msg, is_any_of("|"), token_compress_off);
		if (v.size() != 3)
		{
			data_ = { "ERR\n" };
			return 4;
		}
		char ch = lexical_cast<char>(v[0]);
		switch (ch)
		{
		case 'V':
		{
			if (idwhitelist.find(v[1]) != idwhitelist.end())
			{
				data_ = { "OK\n" };
				nret = 3;
				break;
			}
			// unsigned int ipaddr = ConvertIP(v[2]);
			unsigned long ipaddr = inet_addr(v[2].c_str());
			if (ipaddr != 0)
			{
				if (iplist.find(v[1]) != iplist.end())
				{
					if (iplist[v[1]] == ipaddr)
					{
						data_ = { "OK\n" };
						nret = 3;
					}
					else
					{
						data_ = { "ERR\n" };
						nret = 4;
					}
				}
				else
				{
					zmutex.lock();
					iplist[v[1]] = ipaddr;
					zmutex.unlock();
					data_ = { "OK\n" };
					nret = 3;
				}
			}
			else
			{
				data_ = { "ERR\n" };
				nret = 4;
			}
			break;
		}
		case 'S':
		{
			if (iplist.find(v[1]) != iplist.end())
			{
				string sret = ConvertIPToString(iplist[v[1]]) + "\n";
				std::copy(sret.begin(), sret.end(), data_.begin());
				nret = sret.length();
			}
			else
			{
				data_ = { "None\n" };
				nret = 5;
			}
			break;
		}
		case 'D':
		{
			if (iplist.find(v[1]) != iplist.end())
			{
				zmutex.lock();
				iplist.erase(v[1]);
				zmutex.unlock();
			}
			data_ = { "OK\n" };
			nret = 3;
			break;
		}
		case 'N':
		{
			string sret = lexical_cast<string>(iplist.size()) + "\n";
			std::copy(sret.begin(), sret.end(), data_.begin());
			nret = sret.length();
			break;
		}
		case 'C':
		{
			zmutex.lock();
			iplist.clear();
			zmutex.unlock();
			data_ = { "OK\n" };
			nret = 3;
			break;
		}
		case 'R':
		{
			idwhitelist[v[1]] = 0;
			data_ = { "OK\n" };
			nret = 3;
			break;
		}
		default:
		{
			BOOST_LOG_TRIVIAL(error) << "Unknown protocol:" << ch;
			data_ = { "ERR\n" };
			nret = 4;
			break;
		}
		}
		return nret;
	}

private:
	tcp::socket socket_;
	boost::array<char, 32> data_;
};

class server
{
public:
	server(basio::io_service& io_service, unsigned short port)
		: io_service_(io_service),
		acceptor_(io_service, tcp::endpoint(tcp::v4(), port))
	{
		session* new_session = new session(io_service_);
		acceptor_.async_accept(new_session->socket(),
			boost::bind(&server::handle_accept, this, new_session,
			basio::placeholders::error));
	}

	void handle_accept(session* new_session,
		const boost::system::error_code& ec)
	{
		if (!ec)
		{
			new_session->start();
			new_session = new session(io_service_);
			acceptor_.async_accept(new_session->socket(),
				boost::bind(&server::handle_accept, this, new_session,
				basio::placeholders::error));
		}
		else
		{
			delete new_session;
			BOOST_LOG_TRIVIAL(error) << "Server:" << ec.message();
		}
	}

private:
	basio::io_service& io_service_;
	tcp::acceptor acceptor_;
};

int main(int argc, char **argv)
{
	BOOST_LOG_TRIVIAL(info) << "Starting on port:" << PORT;
	try
	{
		basio::io_service io_service;
		server s(io_service, PORT);
		io_service.run();
	}
	catch (std::exception& e)
	{
		BOOST_LOG_TRIVIAL(fatal) << e.what();
	}
	return 0;
}

