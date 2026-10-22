package org.xphone.client;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import android.util.Base64;

final class TrustStore {
    static File custom(Context context) { return new File(context.getFilesDir(), "custom-ca.pem"); }

    static String export(Context context) throws Exception {
        File target = new File(context.getFilesDir(), "trust.pem");
        try (FileOutputStream out = new FileOutputStream(target)) {
            KeyStore roots = KeyStore.getInstance("AndroidCAStore");
            roots.load(null);
            var aliases = roots.aliases();
            while (aliases.hasMoreElements()) write(out, roots.getCertificate(aliases.nextElement()).getEncoded());
            if (custom(context).exists()) {
                try (var input = new java.io.FileInputStream(custom(context))) {
                    for (var certificate : CertificateFactory.getInstance("X.509").generateCertificates(input))
                        write(out, certificate.getEncoded());
                }
            }
        }
        return target.getAbsolutePath();
    }

    private static void write(FileOutputStream out, byte[] der) throws Exception {
        String pem = "-----BEGIN CERTIFICATE-----\n" + Base64.encodeToString(der, Base64.NO_WRAP)
                + "\n-----END CERTIFICATE-----\n";
        out.write(pem.getBytes(StandardCharsets.US_ASCII));
    }
}
