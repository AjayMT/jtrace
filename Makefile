
JAVA_INCLUDE :=
JAVA_INCLUDE_PLATFORM :=
CPPFLAGS := -Wall -Werror -std=c++17
LDFLAGS :=
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Darwin)
	JAVA_INCLUDE += `/usr/libexec/java_home`/include
	JAVA_INCLUDE_PLATFORM += $(JAVA_INCLUDE)/darwin
	LDFLAGS += -dynamiclib
endif
ifeq ($(UNAME_S), Linux)
	JAVA_INCLUDE += `dirname $(dirname $(dirname $(readlink -f $(which java))))`/include
	JAVA_INCLUDE_PLATFORM += $(JAVA_INCLUDE)/linux
	LDFLAGS += -shared
endif

jtrace: src/jtrace.cpp
	c++ $(CPPFLAGS) -o jtrace $(LDFLAGS) src/jtrace.cpp -I$(JAVA_INCLUDE) -I$(JAVA_INCLUDE_PLATFORM)

.PHONY: clean
clean:
	rm -f jtrace test/*.class
