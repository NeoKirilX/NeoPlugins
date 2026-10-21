package com.mandagram.plugin.monet;

import java.io.File;
import java.awt.image.BufferedImage;
import java.awt.Graphics2D;
import javax.imageio.ImageIO;
import javax.swing.SwingUtilities;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.List;
import sun.misc.Unsafe;

public class MonetWallpaperEngine implements Runnable {
    private static volatile Object sPendingScheme = null;
    private static volatile Object sCustomScheme = null;
    private static volatile long sPendingPacked = 0L;
    private static final MonetWallpaperEngine sInstance = new MonetWallpaperEngine();

    public static Object getActiveCustomScheme() {
        return sCustomScheme;
    }

    public static int extractSeedColor(String filePath) {
        try {
            if (filePath == null || filePath.isEmpty()) return 0;
            File f = new File(filePath);
            if (!f.exists() || !f.isFile()) return 0;
            BufferedImage img = ImageIO.read(f);
            if (img == null) return 0;
            int targetW = 64;
            int targetH = 64;
            BufferedImage scaled = new BufferedImage(targetW, targetH, BufferedImage.TYPE_INT_ARGB);
            Graphics2D g = scaled.createGraphics();
            g.drawImage(img, 0, 0, targetW, targetH, null);
            g.dispose();
            int[] pixels = ((java.awt.image.DataBufferInt) scaled.getRaster().getDataBuffer()).getData();

            ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();
            Class<?> clsCelebi = Class.forName("com.materialkolor.quantize.QuantizerCelebi", true, cl);
            Object celebiInst = clsCelebi.getField("INSTANCE").get(null);
            Method mQuantize = clsCelebi.getMethod("quantize", int[].class, int.class);
            Map<?, ?> map = (Map<?, ?>) mQuantize.invoke(celebiInst, pixels, 128);

            Class<?> clsScore = Class.forName("com.materialkolor.score.Score", true, cl);
            Object scoreInst = clsScore.getField("INSTANCE").get(null);
            Method mScore = clsScore.getMethod("score", Map.class, int.class, Integer.class, boolean.class);
            List<?> list = (List<?>) mScore.invoke(scoreInst, map, 4, Integer.valueOf(0xFF904A32), true);

            if (list != null && !list.isEmpty()) {
                return ((Number) list.get(0)).intValue();
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }
        return 0;
    }

    public static int normalizeSeedColor(int rawArgb, boolean chromaBoost) {
        if (!chromaBoost) return rawArgb;
        try {
            ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();
            Class<?> clsHct = Class.forName("com.materialkolor.hct.Hct", true, cl);
            Object companion = clsHct.getField("Companion").get(null);
            Method mFromInt = companion.getClass().getMethod("fromInt", int.class);
            Object hct = mFromInt.invoke(companion, rawArgb);

            Method mGetHue = clsHct.getMethod("getHue");
            Method mGetChroma = clsHct.getMethod("getChroma");
            double hue = (Double) mGetHue.invoke(hct);
            double chroma = (Double) mGetChroma.invoke(hct);

            double targetChroma = Math.max(chroma, 55.0);
            double targetTone = 52.0;

            Method mFrom = companion.getClass().getMethod("from", double.class, double.class, double.class);
            Object normHct = mFrom.invoke(companion, hue, targetChroma, targetTone);
            Method mToInt = clsHct.getMethod("toInt");
            return (Integer) mToInt.invoke(normHct);
        } catch (Throwable t) {
            return rawArgb;
        }
    }

    public static Object buildColorScheme(long seedColor, boolean isDark, boolean isAmoled, String styleName) {
        try {
            ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();
            Class<?> clsPaletteStyle = Class.forName("com.materialkolor.PaletteStyle", true, cl);
            Object styleObj = clsPaletteStyle.getField("TonalSpot").get(null);
            if ("vibrant".equalsIgnoreCase(styleName)) {
                styleObj = clsPaletteStyle.getField("Vibrant").get(null);
            } else if ("fidelity".equalsIgnoreCase(styleName)) {
                styleObj = clsPaletteStyle.getField("Fidelity").get(null);
            } else if ("neutral".equalsIgnoreCase(styleName)) {
                styleObj = clsPaletteStyle.getField("Neutral").get(null);
            } else if ("expressive".equalsIgnoreCase(styleName)) {
                styleObj = clsPaletteStyle.getField("Expressive").get(null);
            }

            Class<?> clsDyn = Class.forName("com.materialkolor.DynamicColorSchemeKt", true, cl);
            Class<?> clsColor = Class.forName("androidx.compose.ui.graphics.Color", true, cl);
            Class<?> clsFunc1 = Class.forName("kotlin.jvm.functions.Function1", true, cl);

            Method mDyn = clsDyn.getMethod("dynamicColorScheme-mm0v_cE$default",
                long.class, boolean.class, boolean.class,
                clsColor, clsColor, clsColor, clsColor, clsColor, clsColor,
                clsPaletteStyle, double.class, boolean.class,
                clsFunc1,
                int.class, Object.class
            );

            return mDyn.invoke(null,
                seedColor,
                isDark,
                isAmoled,
                null, null, null, null, null, null,
                styleObj,
                0.0,
                false,
                null,
                4088,
                null
            );
        } catch (Throwable t) {
            t.printStackTrace();
            return null;
        }
    }

    public void run() {
        try {
            Object scheme = sPendingScheme;
            long packed = sPendingPacked;
            if (scheme == null) return;
            sCustomScheme = scheme;

            ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();
            Class<?> clsThemeKt = Class.forName("com.mandagram.ui.theme.ThemeKt", true, cl);
            Field fScheme = clsThemeKt.getDeclaredField("ExteraMonetEveningRedDarkScheme");

            Field fUnsafe = Unsafe.class.getDeclaredField("theUnsafe");
            fUnsafe.setAccessible(true);
            Unsafe unsafe = (Unsafe) fUnsafe.get(null);
            Object base = unsafe.staticFieldBase(fScheme);
            long offset = unsafe.staticFieldOffset(fScheme);
            unsafe.putObject(base, offset, scheme);

            try {
                Field fSchemePis = clsThemeKt.getDeclaredField("ExteraMonetPistachioDarkScheme");
                Object basePis = unsafe.staticFieldBase(fSchemePis);
                long offsetPis = unsafe.staticFieldOffset(fSchemePis);
                unsafe.putObject(basePis, offsetPis, scheme);
            } catch (Throwable ignored) {
            }

            Class<?> clsTheme = Class.forName("com.mandagram.ui.theme.ThemeManager", true, cl);
            Object themeInst = clsTheme.getField("INSTANCE").get(null);
            Class<?> clsMonetSeed = Class.forName("com.mandagram.ui.theme.MonetSeed", true, cl);
            Object seedEveningRed = clsMonetSeed.getField("EVENING_RED").get(null);
            Object seedPistachio = clsMonetSeed.getField("PISTACHIO").get(null);
            Method mSetSeed = clsTheme.getMethod("setSeed", clsMonetSeed);

            if (packed != 0L) {
                Method mSetSeedColor = clsTheme.getMethod("setSeedColor-8_81llA", long.class);
                mSetSeedColor.invoke(themeInst, packed);
            }

            mSetSeed.invoke(themeInst, seedPistachio);
            mSetSeed.invoke(themeInst, seedEveningRed);
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }

    public static boolean applyCustomScheme(Object scheme, long packed) {
        if (scheme == null) return false;
        sPendingScheme = scheme;
        sPendingPacked = packed;
        if (SwingUtilities.isEventDispatchThread()) {
            sInstance.run();
        } else {
            SwingUtilities.invokeLater(sInstance);
        }
        return true;
    }

    public static int updateTheme(String filePath, String styleName, boolean isAmoled, boolean chromaBoost) {
        int rawColor = extractSeedColor(filePath);
        if (rawColor == 0) return 0;
        int normColor = normalizeSeedColor(rawColor, chromaBoost);
        long packed = ((long) (normColor & 0xFFFFFFFFL)) << 32;

        ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();
        try {
            Class<?> clsTheme = Class.forName("com.mandagram.ui.theme.ThemeManager", true, cl);
            Object themeInst = clsTheme.getField("INSTANCE").get(null);
            Method mIsDark = clsTheme.getMethod("isDark");
            boolean isDark = (Boolean) mIsDark.invoke(themeInst);

            Object scheme = buildColorScheme(packed, isDark, isAmoled, styleName);
            if (scheme != null) {
                applyCustomScheme(scheme, packed);
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }
        return normColor;
    }
}
