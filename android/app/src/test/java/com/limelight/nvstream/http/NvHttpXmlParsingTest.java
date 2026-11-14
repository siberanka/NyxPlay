package com.limelight.nvstream.http;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.io.StringReader;
import java.util.List;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class NvHttpXmlParsingTest {
    private static String appList(int count, int nameLength) {
        StringBuilder sb = new StringBuilder("<?xml version=\"1.0\" encoding=\"utf-8\"?><root status_code=\"200\">");
        StringBuilder name = new StringBuilder();
        for (int i = 0; i < nameLength; i++) {
            name.append('ç');
        }
        for (int i = 0; i < count; i++) {
            sb.append("<App><IsHdrSupported>0</IsHdrSupported><AppTitle>").append(name).append(i)
                    .append("</AppTitle><ID>").append(i + 1).append("</ID></App>");
        }
        return sb.append("</root>").toString();
    }

    @Test(expected = XmlPullParserException.class)
    public void getXmlString_missingMandatory_throwsXmlPullParser() throws Exception {
        NvHTTP.getXmlString("<root status_code=\"200\"><a>1</a></root>", "b", true);
    }

    @Test
    public void getXmlString_missingOptional_returnsNull() throws Exception {
        assertNull(NvHTTP.getXmlString("<root status_code=\"200\"><a>1</a></root>", "b", false));
    }

    @Test
    public void getXmlString_textBeforeRoot_doesNotThrowRuntimeException() throws Exception {
        try {
            NvHTTP.getXmlString("garbage<root status_code=\"200\"><a>1</a></root>", "a", false);
        } catch (XmlPullParserException | IOException ok) {
            // A parser level error is acceptable, an unchecked exception is not
        }
    }

    @Test
    public void getXmlArray_textBeforeRoot_doesNotThrowRuntimeException() throws Exception {
        try {
            List<String> values = NvHTTP.getXmlArray("garbage<root><a>1</a></root>", "a", false);
            assertTrue(values.size() <= 1);
        } catch (XmlPullParserException | IOException ok) {
            // acceptable
        }
    }

    @Test
    public void appList_300Apps_longNames_parsed() throws Exception {
        assertEquals(300, NvHTTP.getAppListByReader(new StringReader(appList(300, 200))).size());
    }

    @Test
    public void appList_truncated_noRootEnd_throws() throws Exception {
        String xml = appList(2, 5);
        xml = xml.substring(0, xml.length() - "</root>".length());
        try {
            NvHTTP.getAppListByReader(new StringReader(xml));
            fail("Expected an exception for a truncated list");
        } catch (XmlPullParserException expected) {
            // expected
        }
    }

    @Test
    public void appList_incompleteApp_removed() throws Exception {
        String xml = "<root status_code=\"200\"><App><AppTitle>only a name</AppTitle></App>" +
                "<App><AppTitle>ok</AppTitle><ID>5</ID></App></root>";
        assertEquals(1, NvHTTP.getAppListByReader(new StringReader(xml)).size());
    }

    @Test
    public void appList_textBeforeAnyApp_ignored() throws Exception {
        String xml = "<root status_code=\"200\">stray text<App><AppTitle>ok</AppTitle><ID>5</ID></App></root>";
        assertEquals(1, NvHTTP.getAppListByReader(new StringReader(xml)).size());
    }

    @Test
    public void appList_badStatusCode_hostHttpResponseException() throws Exception {
        try {
            NvHTTP.getAppListByReader(new StringReader("<root status_code=\"401\" status_message=\"no\"></root>"));
            fail("Expected HostHttpResponseException");
        } catch (HostHttpResponseException e) {
            assertEquals(401, e.getErrorCode());
        }
    }
}
