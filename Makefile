NAME      := webserv
CXX       := c++
CXXFLAGS  := -Wall -Wextra -Werror -std=c++98
INCLUDES  := -Isrc -Iinclude

SRC_DIR   := src
OBJ_DIR   := obj

SRCS := \
	src/main.cpp \
	src/Config.cpp \
	src/ServerRunner.cpp \
	src/HttpSerializer.cpp \
	src/HttpHeader.cpp \
	src/HttpBody.cpp \

OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

# Colors for pretty output
GREEN   := \033[1;32m
YELLOW  := \033[1;33m
CYAN    := \033[1;36m
RED     := \033[1;31m
RESET   := \033[0m

# Default target: run spinner, then build the binary if needed
all: $(NAME)

# Binary depends ONLY on object files
$(NAME): $(OBJS)
	@printf "$(GREEN)[Link]$(RESET)  $@\n"
	@$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

# Compile rule: .cpp -> obj/*.o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@printf "$(YELLOW)[Compiling]$(RESET) $<\n"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	@printf "$(RED)[Clean]$(RESET)   Removing object files\n"
	@rm -rf $(OBJ_DIR)

fclean: clean
	@printf "$(RED)[Clean]$(RESET)   Removing $(NAME)\n"
	@rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re