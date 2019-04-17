
public class Test {
    private static int state = 0;

    static class MyClass {
        private int myState = 0;

        public void printSelf() {
            System.out.println(this);
        }
    }

    public static void main(String[] args) {
        int f = 12;
        int local = 42;
        ++state;
        MyClass myObject = new MyClass();
        myObject.printSelf();
    }
}
