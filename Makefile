COMPILER = g++
FLAGS = -std=c++14 -O3 -Wall -Werror -Wextra

SO_DEPS = $(shell pkg-config --libs --cflags libSimpleAmqpClient msgpack librabbitmq)
SO_DEPS += -lboost_program_options -lpthread -larmadillo

MAINTAINER = mendonca
SERVICE = sync
VERSION = 1.0
LOCAL_REGISTRY = git.is:5000

all: $(SERVICE) test

clean:
	rm -f $(SERVICE) test

$(SERVICE): $(SERVICE).cpp $(SERVICE).hpp
	$(COMPILER) $^ -o $@ $(FLAGS) $(SO_DEPS)

test: test.cpp
	$(COMPILER) $^ -o $@ $(FLAGS) $(SO_DEPS) 

docker: $(SERVICE)
	rm -rf libs/
	mkdir libs/
	lddcp $(SERVICE) libs/
	docker build -t $(MAINTAINER)/$(SERVICE):$(VERSION) .
	rm -rf libs/
	
push_local: docker
	docker tag $(MAINTAINER)/$(SERVICE):$(VERSION) $(LOCAL_REGISTRY)/$(SERVICE):$(VERSION) 
	docker push $(LOCAL_REGISTRY)/$(SERVICE):$(VERSION)

push_cloud: docker
	docker push $(MAINTAINER)/$(SERVICE):$(VERSION)