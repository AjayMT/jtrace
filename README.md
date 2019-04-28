
# jtrace
`jtrace` is a code tracing tool that records the state of a Java program as it executes.

## Build
```sh
git clone http://github.com/AjayMT/jtrace.git
cd jtrace
make # this will produce a `jtrace` shared library file
```

## Usage
The `jtrace` shared library is a [native agent](https://www.oracle.com/technetwork/articles/javase/index-140680.html) that interacts with the tracee through an instance of `JTraceReceiver` that implements the following interface:
```java
class JTraceReceiver {
    /** Begin tracing. */
    public void start() {
        // pre-trace stuff...
    }

    /** End tracing. */
    public void end() {
        // post-trace stuff...
    }

    /** Receive trace results. */
    public void receive(String result) {
        // process trace results...
    }

    /** ...other fields/methods... */
}
```

For example:
```java
class Test {
    class JTraceReceiver {
        public void start() {}
        public void end() {}
        public void receive(String s) {
            System.out.println(s);
        }
    }

    public static void main(String[] args) {
        JTraceReceiver tracer = new JTraceReceiver();
        tracer.start(); // tracing begins here
        for (int i = 0; i < 10; ++i);
        tracer.end(); // tracing ends here
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
- `jtrace` only records program state when it changes. Execution steps that do not change the program's state are ignored. Whether or not to do this should also be specified by the receiver.
- `jtrace` does not produce trace output during tracing -- all of the output is sent to the receiver when tracing ends. This is for a number of reasons, but mostly because it is non-trivial to keep track of the receiver object as the JVM moves it all over the heap.
- String of TOML is a suboptimal way to send results.
