CXX ?= g++

DEBUG ?= 1

ifeq ($(DEBUG), 1)
	CXXFLAGS += -g
else
	CXXFLAGS += -O2
endif

server: main.cpp ./timer/lst_timer.cpp ./httprequest/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp  ./webserver/webserver.cpp ./webserver/config.cpp
		$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
		rm -r server