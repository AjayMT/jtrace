
import java.util.ArrayList;

public class Receiver {
    private static class JTraceReceiver {
        public ArrayList<String> output = new ArrayList<>();
        public void start() {}
        public void end() {}
        public void add(String s) { output.add(s); }
    }

    public static void main(String args[]) {
        int a = 1;
        JTraceReceiver tracer = new JTraceReceiver();
        tracer.start();
        a = 12;
        tracer.end();
        a = 45;
        System.out.println(tracer.output);
    }
}
