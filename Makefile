NAME      := webserv
CXX       := c++
CXXFLAGS  := -Wall -Wextra -Werror -std=c++98
INCLUDES  := -Isrc -Iinclude

SRCS := \
	src/main.cpp \
	src/Config.cpp \
	src/ServerRunner.cpp \
	src/HttpSerializer.cpp \
	src/HttpParser.cpp \
	src/HttpBody.cpp

OBJS := $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
