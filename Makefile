all: bbserv

bbserv: ./src/server.cpp ./src/server.hpp ./src/io.hpp ./src/defer.hpp ./src/parse.hpp ./src/scope.hpp ./src/config.hpp ./src/daemon.hpp ./src/bullentin_board.hpp ./src/concurrent_queue.hpp ./src/string.hpp
	g++ -std=c++26 -O3 ./src/server.cpp -o bbserv

clean:
	rm bbserv bbfile.txt bbserv.pid bbserv.conf bbserv.log
