servertest:	servertest.cpp Makefile
	g++ -std=c++14 -Wall -O3 servertest.cpp -o servertest -lpthread -g
