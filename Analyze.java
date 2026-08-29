import org.jf.dexlib2.DexFileFactory;
import org.jf.dexlib2.Opcodes;
import org.jf.dexlib2.iface.*;
import org.jf.dexlib2.iface.instruction.*;
import org.jf.dexlib2.iface.instruction.formats.*;
import org.jf.dexlib2.iface.reference.*;
import java.io.File;
import java.util.*;

public class Analyze {
  public static void main(String[] a) throws Exception {
    DexFile dex = DexFileFactory.loadDexFile(new File(a[0]), Opcodes.forApi(29));
    ClassDef eos = null, builder = null, colorParams = null;
    Set<String> browserClasses = new TreeSet<>();
    for (ClassDef c : dex.getClasses()) {
      String t = c.getType();
      if (t.equals("Lcom/epicgames/mobile/eossdk/EOSSDK;")) eos = c;
      if (t.equals("Lb/c/b/c$a;")) builder = c;
      if (t.contains("CustomTabColorSchemeParams")) colorParams = c;
      if (t.startsWith("Landroidx/browser/")) browserClasses.add(t);
    }
    System.out.println("== androidx/browser classes DEFINED in app dex ==");
    for (String s : browserClasses) System.out.println("  " + s);

    System.out.println("\n== methods DEFINED on Lb/c/b/c$a; (obfuscated CustomTabsIntent.Builder?) ==");
    if (builder != null) for (Method m : builder.getMethods())
      System.out.println("  " + m.getName() + params(m) + "->" + m.getReturnType());
    else System.out.println("  (b.c.b.c$a NOT FOUND)");

    System.out.println("\n== CustomTabColorSchemeParams defined? " + (colorParams!=null?colorParams.getType():"NO") + " ==");

    if (eos != null) {
      for (Method m : eos.getMethods()) {
        String n = m.getName();
        if (!(n.equals("PrewarmURL") || n.equals("LaunchURL") || n.toLowerCase().contains("url") || n.toLowerCase().contains("browser") || n.toLowerCase().contains("tab"))) continue;
        MethodImplementation impl = m.getImplementation();
        if (impl == null) continue;
        System.out.println("\n== invokes inside EOSSDK." + n + " ==");
        LinkedHashSet<String> seen = new LinkedHashSet<>();
        for (Instruction ins : impl.getInstructions()) {
          if (ins instanceof ReferenceInstruction) {
            Reference r = ((ReferenceInstruction) ins).getReference();
            if (r instanceof MethodReference) {
              MethodReference mr = (MethodReference) r;
              String dc = mr.getDefiningClass();
              if (dc.startsWith("Landroidx/browser/") || dc.equals("Lb/c/b/c$a;") || dc.startsWith("Lb/c/b/")) {
                seen.add(dc + "->" + mr.getName() + "(" + mr.getParameterTypes() + ")" + mr.getReturnType());
              }
            } else if (r instanceof FieldReference) {
              FieldReference fr = (FieldReference) r;
              if (fr.getDefiningClass().startsWith("Landroidx/browser/") || fr.getDefiningClass().startsWith("Lb/c/b/"))
                seen.add("FIELD " + fr.getDefiningClass() + "->" + fr.getName());
            }
          }
        }
        for (String s : seen) System.out.println("   " + s);
      }
    }
  }
  static String params(Method m){ StringBuilder b=new StringBuilder("("); for(CharSequence p:m.getParameterTypes()) b.append(p); return b.append(")").toString(); }
}
