PROTOC = protoc-c
GENDIR = generated
PROTODIR = protocol

OBJS = trawlerd.o $(GENDIR)/trawler.pb-c.o
SRCS = trawlerd.c $(GENDIR)/trawler.pb-c.c
CFLAGS += -I $(GENDIR) -g -Wall -Wextra -Werror -pedantic -std=gnu99
LDFLAGS += -lzmq -lczmq -lcurl -lprotobuf-c

all: daemon

daemon: $(OBJS)
	$(CC) -o daemon $(CFLAGS) $(OBJS) $(LDFLAGS)

MOCKNS = '"http://localhost:6260/"'

testdaemon: $(SRCS)
	$(CC) -o testdaemon $(CFLAGS) -std=gnu99 -DDEBUG $(SRCS) $(LDFLAGS)

depend: .depend

.depend: $(SRCS)
	rm -f ./.depend
	$(CC) $(CFLAGS) -MM $^ -MF ./.depend

include .depend

$(GENDIR)/%.pb-c.o : $(GENDIR)/%.pb-c.c

$(GENDIR)/%.pb-c.c $(GENDIR)/%.pb-c.h: $(PROTODIR)/%.proto
	mkdir -p $(GENDIR)
	$(PROTOC) --proto_path=$(PROTODIR) --c_out=$(GENDIR) $<

clean:
	rm -f $(GENDIR)/trawler.pb-c.? *.o daemon
