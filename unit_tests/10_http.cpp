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

TEST_CASE( "HTTP GET tests", "[main]" ) {
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
TEST_CASE( "HTTP HEAD tests", "[main]" ) {
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