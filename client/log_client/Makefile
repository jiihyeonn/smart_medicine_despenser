#CC:=arm-linux-gcc
CC:=gcc

TARGET_LOG=log_client
OBJECT_LOG=$(TARGET_LOG).o 

LDFLAGS=-D_REENTRANT -pthread

all : $(TARGET_LOG) 

$(TARGET_LOG): $(OBJECT_LOG)
	$(CC) -o $@ $(OBJECT_LOG) $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(TARGET_LOG)