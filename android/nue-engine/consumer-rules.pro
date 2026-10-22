# Applied to every app that uses the NUE AAR.

# JNI: native code registers natives on this class by name and calls its static methods by name.
-keep class org.nue.engine.NueBridge { *; }
-keepclasseswithmembernames class org.nue.engine.** { native <methods>; }

# Activity / surface classes are referenced from the manifest and by SDL through reflection.
-keep class org.nue.engine.NueActivity { *; }
-keep class org.nue.engine.NueSurface { *; }

# SDL3's Java side is called from native code (methods looked up by name through JNI).
-keep,includedescriptorclasses class org.libsdl.app.** { *; }
-keepclassmembers class org.libsdl.app.** { native <methods>; }
-dontwarn org.libsdl.app.**

# Listener interfaces we hand to system services are anonymous inner classes; keep their names for stack traces.
-keepnames class org.nue.engine.**
