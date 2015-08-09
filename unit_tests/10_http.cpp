#define CATCH_CONFIG_RUNNER
#include <iostream>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include "http_conn.hpp"
#include "catch.hpp"

using namespace std;
boost::asio::io_service io;
boost::asio::yield_context *yield;
boost::asio::ip::tcp::endpoint ep;

TEST_CASE( "HTTP GET tests", "[get]" ) {
	ASIOLibs::HTTP::Conn c( *yield, io, ep );
	c.Headers()["Host"] = "forsakens.ru";
	auto r0 = c.GET("/");
	REQUIRE( r0->status == 200 );
	REQUIRE( r0->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r1 = c.GET("/test/503");
	REQUIRE( r1->status == 503 );
	REQUIRE( r1->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r2 = c.GET("/test/400");
	REQUIRE( r2->status == 400 );
	REQUIRE( r2->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r3 = c.GET("/Pg-hstore-1.01.tar.gz");
	REQUIRE( r3->status == 200 );
	REQUIRE( r3->ContentLength == 123048 );
	REQUIRE( c.getConnCount() == 2 ); //Keepalive drop after code 400
	auto r4 = c.GET("/");
	REQUIRE( r4->status == 200 );
	REQUIRE( c.getConnCount() == 2 );
}
TEST_CASE( "HTTP HEAD tests", "[head]" ) {
	ASIOLibs::HTTP::Conn c( *yield, io, ep );
	c.Headers()["Host"] = "forsakens.ru";
	auto r0 = c.HEAD("/");
	REQUIRE( r0->status == 200 );
	REQUIRE( r0->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r1 = c.HEAD("/test/503");
	REQUIRE( r1->status == 503 );
	REQUIRE( r1->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r2 = c.HEAD("/test/400");
	REQUIRE( r2->status == 400 );
	REQUIRE( r2->ContentLength > 0 );
	REQUIRE( c.getConnCount() == 1 );
	auto r3 = c.HEAD("/Pg-hstore-1.01.tar.gz");
	REQUIRE( r3->status == 200 );
	REQUIRE( r3->ContentLength == 123048 );
	REQUIRE( c.getConnCount() == 2 ); //Keepalive drop after code 400
}
TEST_CASE( "HTTP GET stream tests", "[get]" ) {
	ASIOLibs::HTTP::Conn c( *yield, io, ep );
	c.Headers()["Host"] = "forsakens.ru";
	auto r0 = c.GET("/Pg-hstore-1.01.tar.gz", false);
	REQUIRE( r0->status == 200 );
	REQUIRE( r0->ContentLength == 123048 );
	REQUIRE( c.getConnCount() == 1 );
	size_t cb_read=0;
	std::string cb_read_str;
	c.StreamReadData(r0, [&cb_read_str, &cb_read](const char *buf, size_t len) -> bool {
		cb_read += len;
		cb_read_str += std::string(buf, len);
		return false;
	});
	REQUIRE( cb_read == 123048 );
	auto r1 = c.GET("/Pg-hstore-1.01.tar.gz");
	REQUIRE( r1->status == 200 );
	REQUIRE( r1->ContentLength == 123048 );
	REQUIRE( c.getConnCount() == 1 );
	std::string drain_read_str = r1->drainRead();
	REQUIRE( cb_read_str == drain_read_str );
}
TEST_CASE( "HTTP GET timeouts tests", "[get]" ) {
	{
		ASIOLibs::HTTP::Conn c( *yield, io, ep, 1, 1 ); //Should be connect timeout
		c.Headers()["Host"] = "forsakens.ru";
		bool was_timeout = false;
		try {
			auto r0 = c.GET("/Pg-hstore-1.01.tar.gz");
		}catch(ASIOLibs::HTTP::Timeout &e) {
			was_timeout = true;
		}
		REQUIRE( was_timeout == true );
	}
	{
		ASIOLibs::HTTP::Conn c( *yield, io, ep, 1000, 1 ); //Should be read timeout
		c.Headers()["Host"] = "forsakens.ru";
		bool was_timeout = false;
		try {
			auto r0 = c.GET("/Pg-hstore-1.01.tar.gz");
		}catch(ASIOLibs::HTTP::Timeout &e) {
			was_timeout = true;
		}
		REQUIRE( was_timeout == true );
	}
}

int result=0;
void run_spawn(boost::asio::yield_context _yield, int argc, char* const argv[]) {
	yield = &_yield;
	result = Catch::Session().run( argc, argv );
}

int main( int argc, char* const argv[] ) {
	boost::asio::ip::tcp::resolver resolver(io);
	boost::asio::ip::tcp::resolver::query query("forsakens.ru", "80");
	boost::asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
	ep = *iter;
	boost::asio::spawn( io, boost::bind(run_spawn, _1, argc, argv) );
	io.run();
	return result;
}
