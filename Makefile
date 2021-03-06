CXX = g++
CXXFLAGS = -pthread -std=c++0x
LDFLAGS = -pthread
SOURCE = $(wildcard *.cpp)
OBJECTS = $(SOURCE:.cpp=.o)
TARGET = parcount2

default: $(TARGET)

%.o: %.cpp
	$(CXX)  $(CXXFLAGS) -c -o $@ $< $(LDFLAGS)

parcount: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJECTS) $(TARGET)
