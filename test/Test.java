
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

    public static void main(String[] args) {
        int f = 12;
        int local = 42;
        staticState = !staticState;
        MyClass myObject = new MyClass();
        myObject.printSelf();
    }
}
