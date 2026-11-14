package com.limelight;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

/**
 * Checks the resource files directly (plain JUnit, no Android runtime needed).
 */
public class StringResourcesTest {
    private static final Pattern FORMAT_SPEC = Pattern.compile("%(\\d+\\$)?[sdf]|%\\d*\\.\\d[f]|%\\d+\\$\\.\\d[f]");

    // Strings that credit upstream projects or name third-party server software on purpose
    private static final Set<String> UPSTREAM_NAME_ALLOWED = new HashSet<>(Arrays.asList(
            "summary_about_licenses", "summary_software_update", "searching_pc"));

    private static File resDir() {
        File dir = new File("src/main/res");
        if (!dir.isDirectory()) {
            dir = new File("app/src/main/res");
        }
        assertTrue("res directory not found from " + new File(".").getAbsolutePath(), dir.isDirectory());
        return dir;
    }

    private static Document parse(File f) throws Exception {
        return DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f);
    }

    private static Map<String, String> strings(String folder) throws Exception {
        Map<String, String> out = new LinkedHashMap<>();
        File f = new File(resDir(), folder + "/strings.xml");
        NodeList nodes = parse(f).getElementsByTagName("string");
        for (int i = 0; i < nodes.getLength(); i++) {
            Element e = (Element) nodes.item(i);
            if ("false".equals(e.getAttribute("translatable"))) {
                continue;
            }
            out.put(e.getAttribute("name"), e.getTextContent());
        }
        return out;
    }

    private static List<String> arrayItems(String name) throws Exception {
        NodeList arrays = parse(new File(resDir(), "values/arrays.xml")).getElementsByTagName("string-array");
        for (int i = 0; i < arrays.getLength(); i++) {
            Element e = (Element) arrays.item(i);
            if (name.equals(e.getAttribute("name"))) {
                List<String> items = new ArrayList<>();
                NodeList item = e.getElementsByTagName("item");
                for (int j = 0; j < item.getLength(); j++) {
                    items.add(item.item(j).getTextContent().trim());
                }
                return items;
            }
        }
        throw new AssertionError("array not found: " + name);
    }

    private static List<String> specs(String s) {
        List<String> found = new ArrayList<>();
        Matcher m = FORMAT_SPEC.matcher(s.replace("%%", ""));
        while (m.find()) {
            found.add(m.group());
        }
        java.util.Collections.sort(found);
        return found;
    }

    @Test
    public void turkishDefinesNothingUnknown() throws Exception {
        Set<String> extra = new HashSet<>(strings("values-tr").keySet());
        extra.removeAll(strings("values").keySet());
        assertTrue("Turkish-only keys: " + extra, extra.isEmpty());
    }

    @Test
    public void turkishIsComplete() throws Exception {
        Set<String> missing = new HashSet<>(strings("values").keySet());
        missing.removeAll(strings("values-tr").keySet());
        assertTrue("Missing Turkish strings: " + missing, missing.isEmpty());
    }

    @Test
    public void turkishKeepsTheFormatPlaceholders() throws Exception {
        Map<String, String> en = strings("values");
        Map<String, String> tr = strings("values-tr");
        List<String> problems = new ArrayList<>();
        for (Map.Entry<String, String> e : tr.entrySet()) {
            String source = en.get(e.getKey());
            if (source != null && !specs(source).equals(specs(e.getValue()))) {
                problems.add(e.getKey());
            }
        }
        assertTrue("Placeholder mismatch: " + problems, problems.isEmpty());
    }

    @Test
    public void productNamesAreConsistent() throws Exception {
        List<String> problems = new ArrayList<>();
        for (String folder : new String[]{"values", "values-tr"}) {
            for (Map.Entry<String, String> e : strings(folder).entrySet()) {
                if (UPSTREAM_NAME_ALLOWED.contains(e.getKey())) {
                    continue;
                }
                if (Pattern.compile("Apollo|Moonlight|Artemis").matcher(e.getValue()).find()) {
                    problems.add(folder + "/" + e.getKey());
                }
            }
        }
        assertTrue("Upstream product names in user-visible text: " + problems, problems.isEmpty());
    }

    @Test
    public void noThirdPartyLogMailboxRemains() throws Exception {
        File[] folders = resDir().listFiles(File::isDirectory);
        assertTrue(folders != null && folders.length > 0);
        for (File folder : folders) {
            File f = new File(folder, "strings.xml");
            if (!f.isFile()) {
                continue;
            }
            String text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
            assertFalse(f.getPath(), text.contains("artemistics"));
            assertFalse(f.getPath(), text.contains("tinyurl"));
            assertFalse(f.getPath(), text.contains("name=\"email_recipient\""));
        }
    }

    @Test
    public void turkishCanBeSelected() throws Exception {
        List<String> names = arrayItems("language_names");
        List<String> values = arrayItems("language_values");
        assertEquals(names.size(), values.size());
        int index = values.indexOf("tr");
        assertTrue("tr missing from language_values", index >= 0);
        assertEquals("Türkçe", names.get(index));

        String locales = new String(Files.readAllBytes(new File(resDir(), "xml/locales_config.xml").toPath()), StandardCharsets.UTF_8);
        assertTrue(locales.contains("android:name=\"tr\""));
    }

    @Test
    public void everyLocaleListedInTheConfigHasMatchingLanguageValue() throws Exception {
        List<String> values = arrayItems("language_values");
        NodeList locales = parse(new File(resDir(), "xml/locales_config.xml")).getElementsByTagName("locale");
        for (int i = 0; i < locales.getLength(); i++) {
            String name = ((Element) locales.item(i)).getAttribute("android:name");
            assertTrue("locale not selectable: " + name, values.contains(name));
        }
    }
}
