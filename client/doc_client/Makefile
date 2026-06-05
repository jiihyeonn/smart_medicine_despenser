#CC:=arm-linux-gcc
CC:=gcc

# 타겟 이름을 doc_client로 지정
TARGET_DOC=doc_client
OBJECT_DOC=$(TARGET_DOC).o 

# 의사 클라이언트는 판단 로직과 스레드(pthread)만 사용하므로 무거운 SQL 라이브러리는 제외!
LDFLAGS=-D_REENTRANT -pthread

all : $(TARGET_DOC) 

$(TARGET_DOC): $(OBJECT_DOC)
	$(CC) -o $@ $(OBJECT_DOC) $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $<

clean:
	rm -f *.o $(TARGET_DOC)
