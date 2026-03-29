CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17
SRCDIR = src
BUILDDIR = build

# Source files
SERVER_SOURCES = $(SRCDIR)/server.cpp $(SRCDIR)/hashtable.cpp $(SRCDIR)/protocol.cpp $(SRCDIR)/connection.cpp $(SRCDIR)/kvstore.cpp $(SRCDIR)/buffer.cpp
CLIENT_SOURCES = $(SRCDIR)/client.cpp

# Object files
SERVER_OBJECTS = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(SERVER_SOURCES))
CLIENT_OBJECTS = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(CLIENT_SOURCES))

# Targets
TARGETS = $(BUILDDIR)/server $(BUILDDIR)/client

.PHONY: all clean

all: $(TARGETS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c $< -o $@

$(BUILDDIR)/server: $(SERVER_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/client: $(CLIENT_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -rf $(BUILDDIR)
