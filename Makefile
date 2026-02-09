# Compiler settings
CC = clang
CXX = clang++

# Flags
COMMON_FLAGS = -O3 -march=native -flto -ffast-math -DGDSTK_NO_PYTHON -DGDSTK_CUSTOM_ALLOCATOR
INCLUDES = -Isrc -Ivendor/gdstk/include -Ivendor/gdstk/external -Ivendor/miniz -Isrc/qhull_stub

# Add dependency tracking flags
DEPFLAGS = -MMD -MP

CFLAGS = $(COMMON_FLAGS) $(INCLUDES) $(DEPFLAGS) -std=gnu99
CXXFLAGS = $(COMMON_FLAGS) $(INCLUDES) $(DEPFLAGS) -std=c++17

# Source files
MINIZ_SRC = vendor/miniz/miniz.c
GDSTK_SRCS = $(wildcard vendor/gdstk/src/*.cpp)
CLIPPER_SRC = vendor/gdstk/external/clipper/clipper.cpp

OBJS_COMMON = $(MINIZ_SRC:.c=.o) \
              $(GDSTK_SRCS:.cpp=.o) \
              $(CLIPPER_SRC:.cpp=.o) \
              src/wasm_allocator.o

OBJS = $(OBJS_COMMON) src/main.o


# Dependency files
DEPS = $(OBJS:.o=.d)


# Target executables
TARGETS = explorer


# Rules
.PHONY: all clean

all: $(TARGETS)

explorer: $(OBJS)
	@echo "🚀 Linking explorer..."
	@$(CXX) $(COMMON_FLAGS) $(OBJS) -o explorer
	@du -h explorer


src/main.o: src/main.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@


# Compile C sources
%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ sources
%.o: %.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Include dependency files
-include $(DEPS)

clean:
	@rm -f $(OBJS) $(DEPS) $(TARGETS)

	@find . -name "*.o" -delete
	@find . -name "*.d" -delete
