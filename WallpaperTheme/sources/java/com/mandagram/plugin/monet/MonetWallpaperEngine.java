package com.mandagram.plugin.monet;

import java.io.File;
import java.io.BufferedReader;
import java.io.FileReader;
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

    public static int parseHexColor(String str) {
        if (str == null) return 0;
        str = str.trim();
        if (str.startsWith("#")) {
            str = str.substring(1);
        } else if (str.startsWith("0x") || str.startsWith("0X")) {
            str = str.substring(2);
        }
        try {
            if (str.length() == 6) {
                return (int) (Long.parseLong(str, 16) | 0xFF000000L);
            } else if (str.length() == 8) {
                return (int) Long.parseLong(str, 16);
            }
        } catch (Throwable ignored) {
        }
        return 0;
    }

    public static int readSystemColorFallback() {
        String home = System.getProperty("user.home");
        if (home == null || home.isEmpty()) return 0;

        String[] candidates = {
            home + "/.config/mandagram/wallpaper_color.txt",
            home + "/.local/state/quickshell/user/generated/color.txt",
            home + "/.cache/wal/colors"
        };

        for (String cPath : candidates) {
            try {
                File cf = new File(cPath);
                if (cf.exists() && cf.isFile()) {
                    BufferedReader br = new BufferedReader(new FileReader(cf));
                    String line;
                    while ((line = br.readLine()) != null) {
                        int col = parseHexColor(line);
                        if (col != 0) {
                            br.close();
                            return col;
                        }
                    }
                    br.close();
                }
            } catch (Throwable ignored) {
            }
        }

        try {
            File jf = new File(home + "/.local/state/quickshell/user/generated/colors.json");
            if (jf.exists() && jf.isFile()) {
                BufferedReader br = new BufferedReader(new FileReader(jf));
                String line;
                while ((line = br.readLine()) != null) {
                    if (line.contains("\"primary\":")) {
                        int q1 = line.indexOf('"', line.indexOf("\"primary\":") + 10);
                        int q2 = (q1 != -1) ? line.indexOf('"', q1 + 1) : -1;
                        if (q1 != -1 && q2 != -1) {
                            int col = parseHexColor(line.substring(q1 + 1, q2));
                            if (col != 0) {
                                br.close();
                                return col;
                            }
                        }
                    }
                }
                br.close();
            }
        } catch (Throwable ignored) {
        }

        return 0;
    }

    public static int extractSeedColor(String filePath) {
        try {
            if (filePath != null) {
                int direct = parseHexColor(filePath);
                if (direct != 0) return direct;
            }

            if (filePath != null && !filePath.isEmpty()) {
                File f = new File(filePath);
                if (f.exists() && f.isFile()) {
                    if (filePath.endsWith(".txt") || filePath.endsWith(".colors")) {
                        try (BufferedReader br = new BufferedReader(new FileReader(f))) {
                            String line;
                            while ((line = br.readLine()) != null) {
                                int col = parseHexColor(line);
                                if (col != 0) return col;
                            }
                        }
                    }

                    BufferedImage img = ImageIO.read(f);
                    if (img != null) {
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
                        @SuppressWarnings("unchecked")
                        Map<Integer, Integer> map = (Map<Integer, Integer>) mQuantize.invoke(celebiInst, pixels, 128);

                        if (map != null && !map.isEmpty()) {
                            Class<?> clsScore = Class.forName("com.materialkolor.score.Score", true, cl);
                            Object scoreInst = clsScore.getField("INSTANCE").get(null);
                            Method mScore = clsScore.getMethod("score", Map.class, int.class, Integer.class, boolean.class);
                            List<?> list = (List<?>) mScore.invoke(scoreInst, map, 4, null, true);

                            if (list != null && !list.isEmpty()) {
                                return ((Number) list.get(0)).intValue();
                            }

                            int bestColor = 0;
                            int bestCount = -1;
                            for (Map.Entry<Integer, Integer> entry : map.entrySet()) {
                                if (entry.getValue() > bestCount) {
                                    bestCount = entry.getValue();
                                    bestColor = entry.getKey();
                                }
                            }
                            if (bestColor != 0) return bestColor;
                        }
                    }
                }
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }

        return readSystemColorFallback();
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
            Method mGetTone = clsHct.getMethod("getTone");

            double hue = (Double) mGetHue.invoke(hct);
            double chroma = (Double) mGetChroma.invoke(hct);
            double tone = (Double) mGetTone.invoke(hct);

            if (chroma < 36.0) {
                Method mFrom = companion.getClass().getMethod("from", double.class, double.class, double.class);
                Object normHct = mFrom.invoke(companion, hue, 36.0, tone);
                Method mToInt = clsHct.getMethod("toInt");
                return (Integer) mToInt.invoke(normHct);
            }
            return rawArgb;
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
            if (packed == 0L && scheme == null) return;
            sCustomScheme = scheme;

            ClassLoader cl = MonetWallpaperEngine.class.getClassLoader();

            if (scheme != null) {
                try {
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
                } catch (Throwable ignored) {
                }
            }

            Class<?> clsTheme = Class.forName("com.mandagram.ui.theme.ThemeManager", true, cl);
            Object themeInst = clsTheme.getField("INSTANCE").get(null);

            if (packed != 0L) {
                Method mSetSeedColor = clsTheme.getMethod("setSeedColor-8_81llA", long.class);
                Method mGetSeedColor = clsTheme.getMethod("getSeedColor-0d7_KjU");
                long cur = (Long) mGetSeedColor.invoke(themeInst);
                if (cur == packed) {
                    mSetSeedColor.invoke(themeInst, packed ^ (1L << 32));
                }
                mSetSeedColor.invoke(themeInst, packed);
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }

    public static boolean applyCustomScheme(Object scheme, long packed) {
        if (packed == 0L && scheme == null) return false;
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

        if ((normColor & 0x00FFFFFF) == 0x00E02B37) {
            normColor ^= 1;
        }
        if ((normColor & 0x00FFFFFF) == 0x008E9E3F) {
            normColor ^= 1;
        }

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
