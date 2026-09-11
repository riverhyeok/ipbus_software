# Compiler settings
CXX = g++
CXXFLAGS = -g -Wall -std=c++17

# IPbus / Cactus Paths
CACTUS_ROOT = /opt/cactus
INCLUDE_PATH = -I$(CACTUS_ROOT)/include
LIBRARY_PATH = -L$(CACTUS_ROOT)/lib

# Libraries to link
LIBS = -lcactus_uhal_uhal \
       -lcactus_uhal_log \
       -lcactus_uhal_grammars \
       -lboost_system

# Build all targets
all: test_bandwidth test_bandwidth_reco test_bandwidth_flow test_dummy_reco reco_tdc_analyzer reco_spatial_analyzer

# Build target
test_bandwidth: test_bandwidth.cxx
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATH) $(LIBRARY_PATH) -o test_bandwidth test_bandwidth.cxx $(LIBS)

test_bandwidth_reco: test_bandwidth_reco.cxx
	$(CXX) $(CXXFLAGS) -o test_bandwidth_reco test_bandwidth_reco.cxx

test_bandwidth_flow: test_bandwidth_flow.cxx
	$(CXX) $(CXXFLAGS) -o test_bandwidth_flow test_bandwidth_flow.cxx

# Dummy Reco
test_dummy_reco: test_dummy_reco.cxx
	$(CXX) $(CXXFLAGS) -o test_dummy_reco test_dummy_reco.cxx

# TDC Analyzer (ROOT)
reco_tdc_analyzer: reco_tdc_analyzer.cpp
	$(CXX) -O3 -std=c++17 reco_tdc_analyzer.cpp -o reco_tdc_analyzer `root-config --cflags --glibs`
# TDC spatial Analyzer (ROOT)
reco_spatial_analyzer: reco_spatial_analyzer.cpp
	$(CXX) -O3 -std=c++17 reco_spatial_analyzer.cpp -o reco_spatial_analyzer `root-config --cflags --glibs`

# Clean
clean:
	rm -f test_bandwidth test_bandwidth_reco test_bandwidth_flow test_dummy_reco reco_spatial_analyzer reco_spatial_analyzer

