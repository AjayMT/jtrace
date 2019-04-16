
JAVA_INCLUDE=`/usr/libexec/java_home`/include

all: src/jtrace.cpp
	c++ -Wall -Werror -std=c++17 -o jtrace.dylib -dynamiclib src/jtrace.cpp -I$(JAVA_INCLUDE) -I$(JAVA_INCLUDE)/darwin

.PHONY: clean
clean:
	rm -f jtrace.dylib
