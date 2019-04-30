
class Interfaces {
    static class Sandwich implements Comparable<Sandwich> {
        public int compareTo(Sandwich other) {
            return 0;
        }
    }

    static class JTraceReceiver {
        static boolean filterSteps = true;
        static void start() {}
        static void end() {}
        static void receive(String s, int n) { System.out.println(s); }
    }

    public static void main(String[] args) {
        JTraceReceiver.start();
        Comparable sub = new Sandwich();
        Comparable blt = new Sandwich();
        System.out.println(sub.compareTo(blt));
        JTraceReceiver.end();
    }
}
