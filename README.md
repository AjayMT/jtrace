
# jtrace
`jtrace` is a code tracing tool that records the state of a Java program as it executes.

## Usage
1. Get the source.
```sh
git clone http://github.com/AjayMT/jtrace.git
cd jtrace
make # this will produce a 'jtrace' shared library file
```
2. Compile Java code with the `-g` flag.
```sh
javac -g Example.java
```
3. Set the `JTRACE_OUT` environment variable. This is the path to which `jtrace` will write its output.
```sh
export JTRACE_OUT="jtrace-out.toml" # jtrace produces output in TOML
```
4. Run the Java code with the `-agentpath` argument.
```sh
java -agentpath:./jtrace Example
```
