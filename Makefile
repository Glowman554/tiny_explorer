# Compiler settings
CC = clang
CXX = clang++

# Flags
# -O3 -march=native -flto -ffast-math
COMMON_FLAGS = -O3 -march=native -flto -ffast-math -DGDSTK_NO_PYTHON -DGDSTK_CUSTOM_ALLOCATOR -DHAVE_UNISTD_H -D_DARWIN_C_SOURCE
INCLUDES = -Isrc -Ivendor/gdstk/include -Ivendor/gdstk/external -Ivendor/zlib -Isrc/qhull_stub

# Add dependency tracking flags
DEPFLAGS = -MMD -MP

CFLAGS = $(COMMON_FLAGS) $(INCLUDES) $(DEPFLAGS) -std=gnu99
CXXFLAGS = $(COMMON_FLAGS) $(INCLUDES) $(DEPFLAGS) -std=c++17

# Source files
ZLIB_SRCS = $(wildcard vendor/zlib/*.c)
GDSTK_SRCS = $(wildcard vendor/gdstk/src/*.cpp)
CLIPPER_SRC = vendor/gdstk/external/clipper/clipper.cpp
APP_SRCS = src/main.cpp src/wasm_allocator.cpp

# Object files
OBJS = $(ZLIB_SRCS:.c=.o) \
       $(GDSTK_SRCS:.cpp=.o) \
       $(CLIPPER_SRC:.cpp=.o) \
       $(APP_SRCS:.cpp=.o)

# Dependency files
DEPS = $(OBJS:.o=.d)

# Target executable
TARGET = explorer

# Rules
.PHONY: all clean

all: $(TARGET)
	@echo "✅ Build complete: ./$(TARGET)"
	@du -h $(TARGET)

$(TARGET): $(OBJS)
	@echo "🚀 Linking $(TARGET)..."
	@$(CXX) $(COMMON_FLAGS) $(OBJS) -o $(TARGET)

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
	@rm -f $(OBJS) $(DEPS) $(TARGET)

