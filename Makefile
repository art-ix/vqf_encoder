CXX ?= g++
# Codebook tables are data — skip -O2 / exceptions / RTTI. The rest of the
# library is compiled to separate .o files so encoder.cpp edits do not
# re-parse 575 KB of arrays, and encode/decode share objects.
CXXFLAGS ?= -pthread -O2 -std=c++17 -Wall -Wno-unused-function -pipe
TABLEFLAGS ?= -O0 -g0 -fno-exceptions -fno-rtti -fno-var-tracking -std=c++17 -Wall -Wno-unused-function -pipe
CPPFLAGS ?= -Itwinvq/include -Itwinvq/src
DEPFLAGS = -MMD -MP

LIB_SRCS = twinvq/src/vqf_file.cpp twinvq/src/twinvq_mdct.cpp \
           twinvq/src/twinvq_decoder.cpp twinvq/src/twinvq_encoder.cpp \
           twinvq/src/twinvq_window_test.cpp twinvq/src/twinvq_psychoacoustic.cpp
LIB_OBJS = $(patsubst twinvq/src/%.cpp,obj/%.o,$(LIB_SRCS)) obj/twinvq_tables.o
DECODE_OBJS = obj/vqf_file.o obj/twinvq_mdct.o obj/twinvq_decoder.o obj/twinvq_tables.o
ALL_OBJS = $(LIB_OBJS) obj/vqf_encode.o obj/vqf_decode.o obj/test_vq_state.o

.PHONY: all test clean

all: bin/vqf_encode bin/vqf_decode

obj bin:
	mkdir -p obj bin

obj/%.o: twinvq/src/%.cpp | obj
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

obj/twinvq_tables.o: twinvq/src/twinvq_tables.cpp twinvq/src/twinvq_tables.hpp | obj
	$(CXX) $(TABLEFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

obj/vqf_encode.o: tools/vqf_encode.cpp | obj
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

obj/vqf_decode.o: tools/vqf_decode.cpp | obj
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

bin/vqf_encode: $(LIB_OBJS) obj/vqf_encode.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^

bin/vqf_decode: $(DECODE_OBJS) obj/vqf_decode.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^

test: all
	./bin/vqf_encode --list-modes
	./bin/vqf_encode --test-resample
	./bin/vqf_encode --test-gains
	./bin/vqf_encode --test-mdct
	./bin/vqf_encode --test-codec
	./bin/vqf_encode --test-codec-short
	./bin/vqf_encode --test-codec-medium
	./bin/vqf_encode --test-codec-adaptive
	./bin/vqf_encode --test-codec-time
	./bin/vqf_encode --test-codec-ppc
	./bin/vqf_encode --test-codec-parallel
	python3 tools/test_parallel_options.py bin/vqf_encode
	python3 tools/test_voice_protection.py bin/vqf_encode
	python3 tools/test_tonal_protection.py bin/vqf_encode
	python3 tools/test_stereo_noise_protection.py bin/vqf_encode
	./bin/vqf_encode --test-codec-ppc-time
	./bin/vqf_encode --test-codec-psychoacoustic
	./bin/vqf_encode --test-codec-lsp
	./bin/vqf_encode --test-codec-bark
	./bin/vqf_encode --test-codec-basic
	./bin/vqf_encode --test-roundtrip 0.5

clean:
	rm -rf bin obj

-include $(ALL_OBJS:.o=.d)

# Optional reference/candidate bitstream invariant check.
obj/test_vq_state.o: tools/test_vq_state.cpp | obj
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

bin/test_vq_state: $(LIB_OBJS) obj/test_vq_state.o | bin
	$(CXX) $(CXXFLAGS) -o $@ $^
