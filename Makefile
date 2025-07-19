NAME      := webserv
CXX       := c++
CXXFLAGS  := -Wall -Wextra -Werror -std=c++98

SRCS := \
	src/main.cpp \
	src/Config.cpp \
#     src/SocketManager.cpp \
#     src/EventLoop.cpp \
#     src/RequestParser.cpp \
#     src/ResponseBuilder.cpp \
#     src/StaticFileHandler.cpp \
#     src/CGIExecutor.cpp \
#     src/UploadDelete.cpp \
#     src/VirtualHost.cpp

OBJS := $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re