CXX = clang++
CXXFLAGS += -std=c++14 -Wall
LDFLAGS += -I/usr/local/include -L/usr/local/lib -pthread -lpthread \
	-lprotobuf -lismsgs -lrabbitmq -lSimpleAmqpClient \
	-lboost_system -lboost_program_options
PROTOC = protoc
LOCAL_PROTOS_PATH = ./msgs/

vpath %.proto $(LOCAL_PROTOS_PATH)

MAINTAINER = viros
SERVICE = sync
TEST = test
VERSION = 1
LOCAL_REGISTRY = ninja.local:5000

all: debug

debug: CXXFLAGS += -g 
debug: LDFLAGS += -fsanitize=address -fno-omit-frame-pointer
debug: $(SERVICE) $(TEST)

release: CXXFLAGS += -Werror -O2
release: $(SERVICE)

$(SERVICE): $(SERVICE).o 
	$(CXX) $^ $(LDFLAGS) -o $@

$(TEST): $(TEST).o 
	$(CXX) $^ $(LDFLAGS) -o $@

.PRECIOUS: %.pb.cc
%.pb.cc: %.proto
	$(PROTOC) -I $(LOCAL_PROTOS_PATH) --cpp_out=. $<

clean:
	rm -f *.o *.pb.cc *.pb.h $(SERVICE) $(TEST)

docker: 
	docker build -t $(MAINTAINER)/$(SERVICE):$(VERSION) --build-arg=SERVICE=$(SERVICE) .

push_local: docker
	docker tag $(MAINTAINER)/$(SERVICE):$(VERSION) $(LOCAL_REGISTRY)/$(SERVICE):$(VERSION)
	docker push $(LOCAL_REGISTRY)/$(SERVICE):$(VERSION)

push_cloud: docker
	docker push $(MAINTAINER)/$(SERVICE):$(VERSION)