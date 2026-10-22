package org.xphone.client;

/** JNI calls are confined to CallService's network thread; codec handles belong to audio workers. */
final class NativeBridge {
    static { System.loadLibrary("xphone_android"); }
    interface Listener {
        void onControl(String json);
        void onMedia(byte[] record);
    }
    static native long open(String url, String user, String token, String caPath,
                            boolean http2Only, Listener listener);
    static native void pump(long handle);
    static native boolean send(long handle, String json);
    static native boolean mediaOpen(long handle, long call);
    static native void mediaClose(long handle);
    static native void mediaSend(long handle, byte[] record);
    static native void close(long handle);
    static native long codecCreate(int txChannels, int rxChannels);
    static native byte[] encode(long codec, short[] pcm);
    static native short[] decode(long codec, byte[] packet);
    static native void codecDestroy(long codec);
    private NativeBridge() {}
}
