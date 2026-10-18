# Add project specific ProGuard rules here.
# By default, the flags in this file are appended to flags specified
# in the Android SDK's proguard config. Keep JNI entry points.
-keepclasseswithmembernames class dev.glimpse.motogps.NativeBridge {
    native <methods>;
}
