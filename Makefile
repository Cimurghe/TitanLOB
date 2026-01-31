
CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O3 -march=native -pthread


IMGUI_DIR := third_party/imgui
IMPLOT_DIR := third_party/implot


INCLUDES := -I. \
            -I$(IMGUI_DIR) \
            -I$(IMGUI_DIR)/backends \
            -I$(IMPLOT_DIR)


LIBS := -lglfw -lGL -ldl


IMGUI_SOURCES := $(IMGUI_DIR)/imgui.cpp \
                 $(IMGUI_DIR)/imgui_draw.cpp \
                 $(IMGUI_DIR)/imgui_tables.cpp \
                 $(IMGUI_DIR)/imgui_widgets.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
                 $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp


IMPLOT_SOURCES := $(IMPLOT_DIR)/implot.cpp \
                  $(IMPLOT_DIR)/implot_items.cpp


ENGINE_SOURCES := order_book.cpp


GUI_SOURCES := dashboard_gui.cpp


IMGUI_OBJS := $(IMGUI_SOURCES:.cpp=.o)
IMPLOT_OBJS := $(IMPLOT_SOURCES:.cpp=.o)
ENGINE_OBJS := $(ENGINE_SOURCES:.cpp=.o)
GUI_OBJS := $(GUI_SOURCES:.cpp=.o)

ALL_OBJS := $(IMGUI_OBJS) $(IMPLOT_OBJS) $(ENGINE_OBJS) $(GUI_OBJS)


TARGET := titan_gui


all: $(TARGET)


$(TARGET): $(ALL_OBJS)
	@echo "Linking $@..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Build complete: ./$(TARGET)"


$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(IMGUI_DIR)/backends/%.o: $(IMGUI_DIR)/backends/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@


$(IMPLOT_DIR)/%.o: $(IMPLOT_DIR)/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@


%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@


clean:
	@echo "Cleaning..."
	rm -f $(ALL_OBJS) $(TARGET)
	@echo "✓ Clean complete"


run: $(TARGET)
	./$(TARGET)


check-deps:
	@echo "Checking dependencies..."
	@command -v pkg-config >/dev/null 2>&1 || { echo "✗ pkg-config not found"; exit 1; }
	@pkg-config --exists glfw3 || { echo "✗ GLFW3 not found"; exit 1; }
	@test -f $(IMGUI_DIR)/imgui.h || { echo "✗ ImGui not found in third_party/"; exit 1; }
	@test -f $(IMPLOT_DIR)/implot.h || { echo "✗ ImPlot not found in third_party/"; exit 1; }
	@echo "✓ All dependencies present"

.PHONY: all clean run check-deps
