# Compiler settings
CC = clang
CXX = clang++

# Flags
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

OBJS_COMMON = $(ZLIB_SRCS:.c=.o) \
              $(GDSTK_SRCS:.cpp=.o) \
              $(CLIPPER_SRC:.cpp=.o) \
              src/wasm_allocator.o

OBJS_VGA = $(OBJS_COMMON) src/main_vga.o
OBJS_DFF = $(OBJS_COMMON) src/main_dff.o

# Dependency files
DEPS = $(OBJS_VGA:.o=.d) $(OBJS_DFF:.o=.d)

# Target executables
TARGETS = explorer dff_test

# Rules
.PHONY: all clean

all: $(TARGETS)

explorer: $(OBJS_VGA)
	@echo "🚀 Linking explorer..."
	@$(CXX) $(COMMON_FLAGS) $(OBJS_VGA) -o explorer
	@du -h explorer

dff_test: $(OBJS_DFF)
	@echo "🚀 Linking dff_test..."
	@$(CXX) $(COMMON_FLAGS) $(OBJS_DFF) -o dff_test
	@du -h dff_test

src/main_vga.o: src/main.cpp
	@echo "  CXX     $< (VGA)"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

src/main_dff.o: src/main.cpp
	@echo "  CXX     $< (DFF)"
	@$(CXX) $(CXXFLAGS) -DRUN_DFF -c $< -o $@

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
	@rm -f $(OBJS_VGA) $(OBJS_DFF) $(DEPS) $(TARGETS)
	@find . -name "*.o" -delete
	@find . -name "*.d" -delete
