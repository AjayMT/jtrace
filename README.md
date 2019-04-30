
# jtrace
`jtrace` is a code tracing tool that records the state of a Java program as it executes.

## Build
```sh
git clone http://github.com/AjayMT/jtrace.git
cd jtrace
make # this will produce a `jtrace` shared library file
```

## Usage
The `jtrace` shared library is a [native agent](https://www.oracle.com/technetwork/articles/javase/index-140680.html) that interacts with the tracee through a `JTraceReceiver` class that implements the following interface:
```java
class JTraceReceiver {
    /** Whether to record state changes only. */
    static boolean stateOnly;

    /** Begin tracing. */
    static void start() {
        // pre-trace stuff...
    }

    /** End tracing. */
    static void end() {
        // post-trace stuff...
    }

    /** 
     * Receive trace results.
     *
     * @param result Tracing results serialized into TOML.
     * @param stepCount The number of execution steps recorded.
     */
    static void receive(String result, int stepCount) {
        // process trace results...
    }

    /** ...other fields/methods... */
}
```

For example:
```java
class Test {
    class JTraceReceiver {
        static boolean stateOnly;
        static void start() {}
        static void end() {}
        static void receive(String s) {
            System.out.println(s);
        }
    }

    public static void main(String[] args) {
        JTraceReceiver.stateOnly = true; // only record steps that change state
        JTraceReceiver.start(); // tracing begins here
        for (int i = 0; i < 10; ++i);
        JTraceReceiver.end(); // tracing ends here
    }
}
```

To trace code with `jtrace`:
1. Compile it with the `-g` flag to include debugging symbols in the bytecode.
```sh
javac -g Example.java
```
2. Invoke `java` with the `-agentpath:` argument to use `jtrace`.
```sh
java -agentpath:<PATH TO JTRACE> Example
```

`jtrace` records **local**, **instance** (if applicable) and **class** state at every execution step and serializes output into [TOML](https://github.com/toml-lang/toml) before sending it to the receiver.

### TODOs
- `jtrace` does not trace code inside standard library classes. Which classes/namespaces to ignore should be a part of the `JTraceReceiver` interface.
- `jtrace` does not produce trace output during tracing -- all of the output is sent to the receiver when tracing ends. This is for a number of reasons, but mostly because it is non-trivial to keep track of the receiver object as the JVM moves it all over the heap.
- String of TOML is a suboptimal way to send results.
