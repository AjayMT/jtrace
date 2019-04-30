
public class Test {
    private static boolean staticState = false;
    private int instanceState = 1;

    static class MyClass {
        private short myState = 0;

        public void printSelf() {
            System.out.println(this);
            ++myState;
        }
    }

    static class JTraceReceiver {
        public static boolean stateOnly = true;
        public static void start() {}
        public static void end() {}
        public static void receive(String info, int stepCount) {
            System.out.println(stepCount + " steps:\n" + info);
        }
    }

    public static void main(String[] args) {
        int f = 12;
        int local = 42;
        staticState = !staticState;

        JTraceReceiver.start();
        MyClass myObject = new MyClass();
        myObject.printSelf();
        if (true) {
            String a = "A";
            System.out.println(a);
            myObject = null;
        }
        JTraceReceiver.end();
    }
}
