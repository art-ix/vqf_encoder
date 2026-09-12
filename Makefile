CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused-function
INCLUDES = -Itwinvq/include -Itwinvq/src
SRCS = twinvq/src/vqf_file.cpp twinvq/src/twinvq_mdct.cpp twinvq/src/twinvq_decoder.cpp \
       twinvq/src/twinvq_encoder.cpp twinvq/src/twinvq_tables.cpp

.PHONY: all test clean

all: bin/vqf_encode bin/vqf_decode

bin:
	mkdir -p bin

bin/vqf_encode: $(SRCS) tools/vqf_encode.cpp | bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tools/vqf_encode.cpp $(SRCS)

bin/vqf_decode: twinvq/src/vqf_file.cpp twinvq/src/twinvq_mdct.cpp twinvq/src/twinvq_decoder.cpp \
                twinvq/src/twinvq_tables.cpp tools/vqf_decode.cpp | bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tools/vqf_decode.cpp twinvq/src/vqf_file.cpp \
		twinvq/src/twinvq_mdct.cpp twinvq/src/twinvq_decoder.cpp twinvq/src/twinvq_tables.cpp

test: all
	./bin/vqf_encode --test-mdct
	./bin/vqf_encode --test-roundtrip 0.5

clean:
	rm -rf bin
