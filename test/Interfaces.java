
class Interfaces {
    static class Sandwich implements Comparable<Sandwich> {
        public int compareTo(Sandwich other) {
            return 0;
        }
    }

    static class JTraceReceiver {
        void start() {}
        void end() {}
        void receive(String s) { System.out.println(s); }
    }

    public static void main(String[] args) {
        JTraceReceiver tracer = new JTraceReceiver();
        tracer.start();
        Comparable sub = new Sandwich();
        Comparable blt = new Sandwich();
        System.out.println(sub.compareTo(blt));
        tracer.end();
    }
}
