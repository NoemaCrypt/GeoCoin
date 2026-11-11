package com.geocoin.libgeo;

public final class LibGeo {
    static {
        System.loadLibrary("geo");
        System.loadLibrary("geo_jni");
    }

    private LibGeo() {}

    public static int apiVersion() { return nativeApiVersion(); }
    public static String lastError() { return nativeLastError(); }

    public static boolean verifyAddress(String address) {
        return nativeVerifyAddress(address);
    }

    public static String deriveAddressFromPrivateKey(byte[] privateKey32) {
        return nativeDeriveAddressFromPrivateKey(privateKey32);
    }

    public static byte[] signDigest32Recoverable(byte[] digest32, byte[] privateKey32) {
        return nativeSignDigest32Recoverable(digest32, privateKey32);
    }

    public static boolean verifySignedMessage(byte[] digest32, byte[] signature) {
        return nativeVerifySignedMessage(digest32, signature);
    }

    public static String recoverSignerAddress(byte[] digest32, byte[] signature) {
        return nativeRecoverSignerAddress(digest32, signature);
    }

    public static String multisigCreate(int m, int n, String[] participants) {
        return nativeMultisigCreate(m, n, participants);
    }

    public static boolean multisigVerify(String multisigAddress) {
        return nativeMultisigVerify(multisigAddress);
    }

    public static byte[] txHashFromRaw(byte[] rawTx) {
        return nativeTxHashFromRaw(rawTx);
    }

    public static byte[] txSignInput(byte[] txHash32, long inputIndex, byte[] privateKey32) {
        return nativeTxSignInput(txHash32, inputIndex, privateKey32);
    }

    public static boolean txVerifyInputSignature(byte[] txHash32, long inputIndex, byte[] signature) {
        return nativeTxVerifyInputSignature(txHash32, inputIndex, signature);
    }

    public static String txRecoverInputSignerAddress(byte[] txHash32, long inputIndex, byte[] signature) {
        return nativeTxRecoverInputSignerAddress(txHash32, inputIndex, signature);
    }

    private static native int nativeApiVersion();
    private static native String nativeLastError();
    private static native boolean nativeVerifyAddress(String address);
    private static native String nativeDeriveAddressFromPrivateKey(byte[] privateKey32);
    private static native byte[] nativeSignDigest32Recoverable(byte[] digest32, byte[] privateKey32);
    private static native boolean nativeVerifySignedMessage(byte[] digest32, byte[] signature);
    private static native String nativeRecoverSignerAddress(byte[] digest32, byte[] signature);
    private static native String nativeMultisigCreate(int m, int n, String[] participants);
    private static native boolean nativeMultisigVerify(String multisigAddress);
    private static native byte[] nativeTxHashFromRaw(byte[] rawTx);
    private static native byte[] nativeTxSignInput(byte[] txHash32, long inputIndex, byte[] privateKey32);
    private static native boolean nativeTxVerifyInputSignature(byte[] txHash32, long inputIndex, byte[] signature);
    private static native String nativeTxRecoverInputSignerAddress(byte[] txHash32, long inputIndex, byte[] signature);
}
