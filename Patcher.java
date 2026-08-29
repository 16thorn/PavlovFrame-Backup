import org.jf.dexlib2.*;
import org.jf.dexlib2.iface.*;
import org.jf.dexlib2.immutable.*;
import org.jf.dexlib2.immutable.instruction.*;
import java.io.File;
import java.util.*;

public class Patcher {
  public static void main(String[] a) throws Exception {
    String in = a[0], out = a[1];
    DexFile dex = DexFileFactory.loadDexFile(new File(in), Opcodes.forApi(29));
    List<ClassDef> classes = new ArrayList<>();
    boolean patched = false;
    for (ClassDef c : dex.getClasses()) {
      if (c.getType().equals("Lb/c/b/c$a;")) {
        List<Method> methods = new ArrayList<>();
        for (Method m : c.getMethods()) methods.add(m);
        // public CustomTabsIntent.Builder setDefaultColorSchemeParams(CustomTabColorSchemeParams p){ return this; }
        MethodImplementation impl = new ImmutableMethodImplementation(
            2,
            Collections.singletonList(new ImmutableInstruction11x(Opcode.RETURN_OBJECT, 0)),
            null, null);
        Method nm = new ImmutableMethod(
            "Lb/c/b/c$a;",
            "setDefaultColorSchemeParams",
            Collections.singletonList(new ImmutableMethodParameter(
                "Landroidx/browser/customtabs/CustomTabColorSchemeParams;", null, null)),
            "Lb/c/b/c$a;",
            AccessFlags.PUBLIC.getValue(),
            null, null, impl);
        methods.add(nm);
        c = new ImmutableClassDef(c.getType(), c.getAccessFlags(), c.getSuperclass(),
            c.getInterfaces(), c.getSourceFile(), c.getAnnotations(), c.getFields(), methods);
        patched = true;
        System.out.println("patched Lb/c/b/c$a; -> added setDefaultColorSchemeParams");
      }
      classes.add(c);
    }
    if (!patched) { System.out.println("ERROR: Lb/c/b/c$a; not found"); System.exit(2); }
    DexFileFactory.writeDexFile(out, new ImmutableDexFile(Opcodes.forApi(29), classes));
    System.out.println("wrote " + out);
  }
}
