#include <iostream>
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <fcntl.h>
#include "perf.hpp"
#include "utils.hpp"
#include "http_conn.hpp"
#include "stopwatch.hpp"

#define DEBUG(x) //std::cerr << x << std::endl;

namespace ASIOLibs {
namespace HTTP {

extern "C" {
#include "picohttpparser.h"
};

#define TIMEOUT_START(x) \
	boost::system::error_code error_code; \
	setupTimeout( x );

#define TIMEOUT_END() \
		if( error_code ) { \
			if( error_code != boost::asio::error::operation_aborted ) \
				throw boost::system::system_error(error_code); \
			is_timeout=true; \
		} \
		checkTimeout();

#define TIMING_STAT_START(name) \
		{ \
		std::unique_ptr<StopWatch> sw; \
		if( stat ) \
			sw.reset( new StopWatch(name, stat) );

#define TIMING_STAT_END() \
		}

enum {
	max_send_try = 5,
	max_read_transfer = 4096*4
};

Conn::Conn(boost::asio::yield_context &_yield, boost::asio::io_service &_io,
		boost::asio::ip::tcp::endpoint &_ep, long _conn_timeout, long _read_timeout)
		: yield(_yield), ep(_ep), sock(_io), timer(_io), conn_timeout(_conn_timeout),
			read_timeout(_read_timeout), conn_count(0), is_timeout(false), headers_cache_clear(true),
			must_reconnect(true), stat(nullptr) {
	headers["User-Agent"] = "ASIOLibs " ASIOLIBS_VERSION;
	headers["Connection"] = "Keep-Alive";
}

void Conn::checkConnect() {
	if( unlikely(!sock.is_open() || must_reconnect) ) {
		close();
		TIMEOUT_START( conn_timeout );
		TIMING_STAT_START("http_connect");
		sock.async_connect(ep, yield[error_code]);
		TIMING_STAT_END();
		TIMEOUT_END();
		conn_count++;
		must_reconnect=false;
	}
}
void Conn::reconnect() {
	close();
	checkConnect();
}
void Conn::close() {
	if( likely(sock.is_open()) ) sock.close();
}
void Conn::setupTimeout(long milliseconds) {
	is_timeout=false;
	timer.expires_from_now( boost::posix_time::milliseconds( milliseconds ) );
	timer.async_wait( boost::bind(&Conn::onTimeout, this, boost::asio::placeholders::error) );
}
void Conn::onTimeout(const boost::system::error_code &ec) {
	if( likely(!ec && timer.expires_from_now() <= boost::posix_time::seconds(0)) ) {
		close();
		is_timeout=true;
	}
}
void Conn::checkTimeout() {
	timer.cancel();
	if( unlikely(is_timeout) ) {
		close();
		throw Timeout( "ASIOLibs::HTTP::Conn: Timeout while requesting " + ep.address().to_string() );
	}
}

void Conn::headersCacheCheck() {
	if( headers_cache_clear ) {
		headers_cache.clear();
		headers_cache_clear=false;
		for( const auto &i : headers ) {
			headers_cache += i.first;
			headers_cache += ": ";
			headers_cache += i.second;
			headers_cache += "\n";
		}
		headers_cache += "\n";
	}
}

std::unique_ptr< Response > Conn::DoSimpleRequest(const char *cmd, const std::string &uri, bool full_body_read) {
	headersCacheCheck();
	std::string req = ASIOLibs::string_sprintf("%s %s HTTP/1.1\n%s", cmd, uri.c_str(), headers_cache.c_str());
	writeRequest( req.data(), req.size(), true );
	return ReadAnswer(full_body_read);
}

std::unique_ptr< Response > Conn::GET(const std::string &uri, bool full_body_read) {
	return DoSimpleRequest("GET", uri, full_body_read);
}
std::unique_ptr< Response > Conn::HEAD(const std::string &uri) {
	auto ret = DoSimpleRequest("HEAD", uri, false);
	ret->ReadLeft = 0; //Body wont follow
	return ret;
}
std::unique_ptr< Response > Conn::MOVE(const std::string &uri_from, const std::string &uri_to, bool allow_overwrite) {
	headersCacheCheck();
	std::string req = ASIOLibs::string_sprintf("MOVE %s HTTP/1.1\nDestination: %s\nOverwrite: %s\n%s",
		uri_from.c_str(), uri_to.c_str(), (allow_overwrite ? "T" : "F"), headers_cache.c_str());
	writeRequest( req.data(), req.size(), true );
	return ReadAnswer(true);
}

std::unique_ptr< Response > Conn::DoPostRequest(const char *cmd, const std::string &uri, size_t ContentLength,
		std::function< bool(const char **buf, size_t *len) > getDataCallback, bool can_recall) {
	headersCacheCheck();
	std::string req = ASIOLibs::string_sprintf("%s %s HTTP/1.1\nContent-Length: %zu\n%s", cmd, uri.c_str(), ContentLength, headers_cache.c_str());

	int try_count = 0;
	boost::system::error_code error_code;
	while( try_count++ < max_send_try ) {
		checkConnect();
		writeRequest( req.data(), req.size(), false );
		bool is_done = false;
		while( !is_done ) {
			const char *postdata;
			size_t postlen;
			is_done = getDataCallback(&postdata, &postlen);
			if( postdata == NULL )
				ReadAnswer(true);
			TIMING_STAT_START("http_write");
			boost::asio::async_write(sock, boost::asio::buffer(postdata, postlen), yield[error_code]);
			TIMING_STAT_END();
			if( unlikely(!can_recall && error_code) )
				throw boost::system::system_error(error_code);
			if( unlikely(error_code) )
				break;
		}
		if( unlikely(error_code == boost::asio::error::operation_aborted) )
			throw boost::system::system_error(error_code);
		if( likely(!error_code) )
			return ReadAnswer(true);
		if( unlikely(sock.is_open()) ) //Dunno wtf happend, just reconnect
			reconnect();
	}
	throw boost::system::system_error(error_code);
}

std::unique_ptr< Response > Conn::POST(const std::string &uri, const char *postdata, size_t postlen, const char *cmd) {
	auto cb = [&postdata, &postlen](const char **buf, size_t *len) {
		*buf = postdata;
		*len = postlen;
		return true;
	};
	return DoPostRequest(cmd, uri, postlen, cb, true);
}

void Conn::writeRequest(const char *buf, size_t sz, bool wait_read) {
	int try_count = 0;
	boost::system::error_code error_code;
	DEBUG( "writeRequest " << std::string(buf, sz) );
	while( try_count++ < max_send_try ) {
		checkConnect();
		TIMING_STAT_START("http_write");
		boost::asio::async_write(sock, boost::asio::buffer(buf, sz), yield[error_code]);
		TIMING_STAT_END();
		if( likely(!error_code) ) {
			if( wait_read ) {
				setupTimeout( read_timeout );
				TIMING_STAT_START("http_read");
				boost::asio::async_read(sock, boost::asio::null_buffers(), yield[error_code]);
				TIMING_STAT_END();
				TIMEOUT_END();
			}
			if( likely(!error_code) )
				return;
		}
		if( unlikely(sock.is_open()) ) //Dunno wtf happend, just reconnect
			reconnect();
	}
	throw boost::system::system_error(error_code);
}

std::unique_ptr< Response > Conn::ReadAnswer(bool read_body) {
	static const char hdr_end_pattern[] = "\r\n\r\n";
	std::unique_ptr< Response > ret( new Response() );
	ret->ContentLength = -1;
	ret->ReadLeft = 0;

	TIMEOUT_START( read_timeout );
	TIMING_STAT_START("http_read");
	boost::asio::async_read_until(sock, ret->read_buf, std::string(hdr_end_pattern), yield[error_code]);
	TIMING_STAT_END();
	TIMEOUT_END();

	const char *data = boost::asio::buffer_cast<const char *>( ret->read_buf.data() );
	size_t sz = ret->read_buf.size();
	DEBUG("ReadAnswer: " << std::string(data, sz));
	const char *hdr_end = (const char *)memmem(data, sz, hdr_end_pattern, sizeof(hdr_end_pattern)-1);
	if( unlikely(!hdr_end) )
		throw std::runtime_error("Cant find headers end");

	//Parse respnse status
	struct phr_header headers[100];
	size_t num_headers = sizeof(headers) / sizeof(headers[0]);
	int minor_version;
	const char *msg;
	size_t msg_len;
	int pret = phr_parse_response(data, hdr_end-data+sizeof(hdr_end_pattern)-1, &minor_version, &ret->status, &msg, &msg_len, headers, &num_headers, 0);

	if( unlikely(pret != -2 && pret <= 0) )
		throw std::runtime_error("Cant parse http response: " + std::to_string(pret));

	bool l_must_reconnect = true;
	for(size_t i=0; i < num_headers; i++) {
		std::string hdr_name(headers[i].name, headers[i].name_len);
		std::transform(hdr_name.begin(), hdr_name.end(), hdr_name.begin(), ::toupper);

		if( hdr_name == "CONTENT-LENGTH" ) {
			ret->ContentLength = strtol(headers[i].value, NULL, 0);
		}else if( hdr_name == "CONNECTION" && !strncasecmp(headers[i].value, "Keep-Alive", headers[i].value_len) ) {
			l_must_reconnect = false;
		}
		ret->headers.emplace_back( std::move(hdr_name), std::string(headers[i].value, headers[i].value_len) );
	}
	must_reconnect = must_reconnect || l_must_reconnect;
	std::sort(ret->headers.begin(), ret->headers.end());

	ret->read_buf.consume( hdr_end - data + sizeof(hdr_end_pattern)-1 ); //Remove headers from input stream

	if( likely(ret->ContentLength>=0) ) {
		assert( static_cast<size_t>(ret->ContentLength) >= ret->read_buf.size() );
		ret->ReadLeft = ret->ContentLength - ret->read_buf.size();
	}
	if( read_body && ret->ContentLength>0 ) {
		while( ret->ReadLeft > 0 ) {
			TIMEOUT_START( read_timeout );
			size_t rd;
			TIMING_STAT_START("http_read");
			rd = boost::asio::async_read(sock, ret->read_buf,
				boost::asio::transfer_exactly(ret->ReadLeft>max_read_transfer ? max_read_transfer : ret->ReadLeft), yield[error_code]);
			TIMING_STAT_END();
			TIMEOUT_END();
			ret->ReadLeft -= rd;
		}
	}

	return ret;
}

void Conn::PrelaodBytes( std::unique_ptr< Response > &resp, size_t count ) {
	size_t len = resp->read_buf.size();
	if( len >= count && count != 0 )
		return; //Already have this count

	if( unlikely(count == 0 || count > (resp->ReadLeft + len)) )
		count = (resp->ReadLeft + len);
	len = count - len;
	TIMEOUT_START( read_timeout );
	size_t rd;
	TIMING_STAT_START("http_read");
	rd = boost::asio::async_read(sock, resp->read_buf, boost::asio::transfer_exactly(len), yield[error_code]);
	TIMING_STAT_END();
	TIMEOUT_END();
	resp->ReadLeft -= rd;
}

void Conn::StreamReadData( std::unique_ptr< Response > &resp, std::function< size_t(const char *buf, size_t len) > dataCallback, bool disable_drain ) {
	bool interrupt = false, disable_callback=false;
	const char *buf;
	size_t len;
	while( !interrupt ) {
		len = resp->read_buf.size();
		if( likely(len>0) ) {
			buf = boost::asio::buffer_cast<const char *>( resp->read_buf.data() );
			size_t consume_len = len;
			if( ! disable_callback ) {
				consume_len = dataCallback(buf, len);
				if( consume_len==0 ) disable_callback=true;
			}
			resp->read_buf.consume(consume_len);
		}
		if( unlikely(disable_callback && disable_drain) )
			return; //Leave socket unread
		if( likely(resp->ReadLeft > 0) ) {
			while( resp->ReadLeft > 0 ) {
				TIMEOUT_START( read_timeout );
				size_t rd;
				TIMING_STAT_START("http_read");
				rd = boost::asio::async_read(sock, resp->read_buf,
					boost::asio::transfer_exactly(resp->ReadLeft>max_read_transfer ? max_read_transfer : resp->ReadLeft), yield[error_code]);
				TIMING_STAT_END();
				TIMEOUT_END();
				resp->ReadLeft -= rd;
			}
		}else{
			interrupt = true;
		}
	}
}

void Conn::StreamSpliceData( std::unique_ptr< Response > &resp, boost::asio::ip::tcp::socket &dest ) {
#ifndef SPLICE_F_MOVE
	assert( !"Cant call ASIOLibs::HTTP::Conn::StreamSpliceData: splice(2) is linux-only call" );
	abort();
#else
	if( resp->read_buf.size() ) {
		TIMING_STAT_START("client_write");
		boost::asio::async_write(dest, resp->read_buf, yield);
		TIMING_STAT_END();
	}
	while( resp->ReadLeft > 0 ) {
		ssize_t rd = splice( sock.native_handle(), NULL, dest.native_handle(), NULL, resp->ReadLeft, SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_MORE);
		if( unlikely(rd == -1 && errno != EAGAIN) )
			throw std::runtime_error( std::string("splice() failed: ") + strerror(errno) );
		else if( rd < 1 ) {
			TIMEOUT_START( read_timeout );
			TIMING_STAT_START("http_read");
			boost::asio::async_read(sock, boost::asio::null_buffers(), yield[error_code]); //Have somthing to read
			TIMING_STAT_END();
			TIMEOUT_END();
			TIMING_STAT_START("client_write");
			boost::asio::async_write(dest, boost::asio::null_buffers(), yield); //Can write
			TIMING_STAT_END();
		} else
			resp->ReadLeft -= rd;
	}
#endif
}

bool Conn::WriteRequestHeaders(const char *cmd, const std::string &uri, size_t ContentLength) try {
	headersCacheCheck();
	std::string req = ASIOLibs::string_sprintf("%s %s HTTP/1.1\nContent-Length: %zu\n%s",
		cmd, uri.c_str(), ContentLength, headers_cache.c_str());
	writeRequest( req.data(), req.size(), false );
	return true;
} catch (std::exception &e) {
	close();
	throw;
}

bool Conn::WriteRequestData(const void *buf, size_t len) try {
	size_t wr;
	TIMING_STAT_START("http_write");
	wr = boost::asio::async_write(sock, boost::asio::buffer(buf, len), yield);
	TIMING_STAT_END();
	assert( wr == len );
	return true;
} catch (std::exception &e) {
	close();
	throw;
}

size_t Conn::WriteTee(boost::asio::ip::tcp::socket &sock_from, size_t max_bytes) {
#ifndef SPLICE_F_MOVE
	assert( !"Cant call ASIOLibs::HTTP::Conn::WriteTee: tee(2) is linux-only call" );
	abort();
#else
	TIMEOUT_START( read_timeout );
	TIMING_STAT_START("http_write");
	boost::asio::async_read(sock, boost::asio::null_buffers(), yield[error_code]); //Have somthing to read
	TIMING_STAT_END();
	TIMEOUT_END();
	TIMING_STAT_START("client_read");
	boost::asio::async_write(sock_from, boost::asio::null_buffers(), yield); //Can write
	TIMING_STAT_END();
	ssize_t wr = tee(sock_from.native_handle(), sock.native_handle(), max_bytes, SPLICE_F_MORE | SPLICE_F_NONBLOCK);
	if( unlikely(wr == -1) )
		throw std::runtime_error( std::string("tee() failed: ") + strerror(errno) );
	return wr;
#endif
}

size_t Conn::WriteSplice(boost::asio::ip::tcp::socket &sock_from, size_t max_bytes) {
#ifndef SPLICE_F_MOVE
	assert( !"Cant call ASIOLibs::HTTP::Conn::WriteSplice: splice(2) is linux-only call" );
	abort();
#else
	size_t wr_total=0;
	while( max_bytes > wr_total ) {
		ssize_t wr = splice( sock_from.native_handle(), NULL, sock.native_handle(), NULL, max_bytes-wr_total, SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_MORE);
		if( unlikely(wr == -1 && errno != EAGAIN) )
			throw std::runtime_error( std::string("splice() failed: ") + strerror(errno) );
		else if( wr < 1 ) {
			TIMEOUT_START( read_timeout );
			TIMING_STAT_START("http_write");
			boost::asio::async_read(sock, boost::asio::null_buffers(), yield[error_code]); //Have somthing to read
			TIMING_STAT_END();
			TIMEOUT_END();
			TIMING_STAT_START("client_read");
			boost::asio::async_write(sock_from, boost::asio::null_buffers(), yield); //Can write
			TIMING_STAT_END();
		} else
			wr_total += wr;
	}
	return wr_total;
#endif
}


std::string Response::Dump() const {
	ASIOLibs::StrFormatter s;
	s << "HTTP status=" << status << "; ContentLength=" << ContentLength << "; ReadLeft=" << ReadLeft << " Headers:\n";
	for( auto &i : headers ) {
		s << "'" << i.first << "': '" << i.second << "'\n";
	}
	return s.str();
}
std::string Response::drainRead() const {
	const char *data = boost::asio::buffer_cast<const char *>( read_buf.data() );
	size_t sz = read_buf.size();
	std::string ret(data, sz);
	read_buf.consume(sz);
	return ret;
}

const std::string *Response::GetHeader(const std::string &name) {
	auto it = std::lower_bound(headers.begin(), headers.end(), std::make_pair(name, std::string()) );
	if( likely(it->first == name) )
		return &it->second;
	return nullptr;
}

};};
