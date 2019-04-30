
class Filtered {
    static class JTraceReceiver {
        public static boolean stateOnly = false;

        public static void start() {}
        public static void end() {}
        public static void receive(String s, int n) {
            if (stateOnly) System.out.printf("filtered ");
            System.out.println("steps: " + n);
            if (stateOnly) System.out.println(s);
        }
    }

    public static void main(String[] args) {
        JTraceReceiver.start();
        for (int i = 0; i < 10; ++i);
        JTraceReceiver.end();
        JTraceReceiver.stateOnly = true;
        JTraceReceiver.start();
        for (int i = 0; i < 10; ++i);
        JTraceReceiver.end();
    }
}
