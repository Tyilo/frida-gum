/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#define SCRIPT_SUITE "/ObjC"
#include "script-fixture.c"

#import <Foundation/Foundation.h>

TEST_LIST_BEGIN (script_darwin)
  SCRIPT_TESTENTRY (classes_can_be_enumerated)
  SCRIPT_TESTENTRY (object_enumeration_should_contain_parent_methods)
  SCRIPT_TESTENTRY (class_enumeration_should_not_contain_instance_methods)
  SCRIPT_TESTENTRY (instance_enumeration_should_not_contain_class_methods)
  SCRIPT_TESTENTRY (class_can_be_retrieved)
  SCRIPT_TESTENTRY (class_method_can_be_invoked)
  SCRIPT_TESTENTRY (object_can_be_constructed_from_pointer)
  SCRIPT_TESTENTRY (string_can_be_constructed)
  SCRIPT_TESTENTRY (method_implementation_can_be_overridden)
  SCRIPT_TESTENTRY (attempt_to_access_an_inexistent_method_should_throw)
  SCRIPT_TESTENTRY (methods_with_weird_names_can_be_invoked)
  SCRIPT_TESTENTRY (method_call_preserves_value)
  SCRIPT_TESTENTRY (objects_can_be_serialized_to_json)
  SCRIPT_TESTENTRY (performance)
TEST_LIST_END ()

SCRIPT_TESTCASE (classes_can_be_enumerated)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var numClasses = Object.keys(ObjC.classes).length;"
        "send(numClasses > 100);"
        "var count = 0;"
        "for (var className in ObjC.classes) {"
          "if (ObjC.classes.hasOwnProperty(className)) {"
            "count++;"
          "}"
        "}"
        "send(count === numClasses);");
    EXPECT_SEND_MESSAGE_WITH ("true");
    EXPECT_SEND_MESSAGE_WITH ("true");
  }
}

SCRIPT_TESTCASE (object_enumeration_should_contain_parent_methods)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var keys = Object.keys(ObjC.classes.NSDate);"
        "send(keys.includes(\"conformsToProtocol_\"));");
    EXPECT_SEND_MESSAGE_WITH ("true");
  }
}

SCRIPT_TESTCASE (class_enumeration_should_not_contain_instance_methods)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var keys = Object.keys(ObjC.classes.NSDate);"
        "send(keys.includes(\"dateWithTimeIntervalSinceNow_\"));"
        "send(keys.includes(\"initWithTimeIntervalSinceReferenceDate_\"));");
    EXPECT_SEND_MESSAGE_WITH ("true");
    EXPECT_SEND_MESSAGE_WITH ("false");
  }
}

SCRIPT_TESTCASE (instance_enumeration_should_not_contain_class_methods)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var keys = Object.keys(ObjC.classes.NSDate.date());"
        "send(keys.includes(\"initWithTimeIntervalSinceReferenceDate_\"));"
        "send(keys.includes(\"dateWithTimeIntervalSinceNow_\"));");
    EXPECT_SEND_MESSAGE_WITH ("true");
    EXPECT_SEND_MESSAGE_WITH ("false");
  }
}

SCRIPT_TESTCASE (class_can_be_retrieved)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var NSDate = ObjC.classes.NSDate;"
        "send(NSDate instanceof ObjC.Object);"
        "send(\"NSDate\" in ObjC.classes);");
    EXPECT_SEND_MESSAGE_WITH ("true");
    EXPECT_SEND_MESSAGE_WITH ("true");
  }
}

SCRIPT_TESTCASE (class_method_can_be_invoked)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var NSDate = ObjC.classes.NSDate;"
        "var now = NSDate.date();"
        "send(now instanceof ObjC.Object);");
    EXPECT_SEND_MESSAGE_WITH ("true");
  }
}

SCRIPT_TESTCASE (object_can_be_constructed_from_pointer)
{
  @autoreleasepool
  {
    NSString * str = [NSString stringWithUTF8String:"Badger"];

    COMPILE_AND_LOAD_SCRIPT (
        "var str = new ObjC.Object(" GUM_PTR_CONST ");"
        "send(str.toString());",
        str);
    EXPECT_SEND_MESSAGE_WITH ("\"Badger\"");
  }
}

SCRIPT_TESTCASE (string_can_be_constructed)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var NSString = ObjC.classes.NSString;"
        "NSString.stringWithUTF8String_(Memory.allocUtf8String(\"Snakes\"));");
    EXPECT_NO_MESSAGES ();
  }
}

SCRIPT_TESTCASE (method_implementation_can_be_overridden)
{
  @autoreleasepool
  {
    NSString * str = [NSString stringWithUTF8String:"Badger"];

    COMPILE_AND_LOAD_SCRIPT (
        "var NSString = ObjC.classes.NSString;"
        "var method = NSString[\"- description\"];"
        "method.implementation ="
            "ObjC.implement(method, function (handle, selector) {"
                "return NSString.stringWithUTF8String_(Memory.allocUtf8String(\"Snakes\"));"
            "});");
    EXPECT_NO_MESSAGES ();

    NSString * desc = [str description];
    EXPECT_NO_MESSAGES ();

    g_assert_cmpstr (desc.UTF8String, ==, "Snakes");
  }
}

SCRIPT_TESTCASE (attempt_to_access_an_inexistent_method_should_throw)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT ("ObjC.classes.NSDate.snakesAndMushrooms();");
    EXPECT_ERROR_MESSAGE_WITH (ANY_LINE_NUMBER,
        "Error: Unable to find method 'snakesAndMushrooms'");
  }
}

@interface FridaTest1 : NSObject
+ (int)foo_;
+ (int)fooBar_;
+ (int)fooBar:(int)a;
+ (int):(int)a;
+ (int):(int)a :(int)b;
@end

@implementation FridaTest1
+ (int)foo_ {
  return 1;
}
+ (int)fooBar_ {
  return 2;
}
+ (int)fooBar:(int)a {
  return 3;
}
+ (int):(int)a {
  return 4;
}
+ (int):(int)a :(int)b {
  return 5;
}
@end

SCRIPT_TESTCASE (methods_with_weird_names_can_be_invoked)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var FridaTest1 = ObjC.classes.FridaTest1;"
        "var methodNames = ['foo_', 'fooBar_', 'fooBar:', ':', '::'];"
        "var args = [0, 0, 1, 1, 2];"
        "for (var i = 0; i < methodNames.length; i++) {"
            "var m = FridaTest1['+ ' + methodNames[i]];"
            "var val = m.apply(FridaTest1, args[i] == 0? []: args[i] == 1? [0]: [0, 0]);"
            "send(val == i + 1);"
        "}");

    for (gint i = 0; i != 5; i++)
    {
      EXPECT_SEND_MESSAGE_WITH ("true");
    }
  }
}

@interface FridaTest2 : NSObject
@end

#define METHOD(t, n) + (t)_ ## n:(t)x { return x; }
@implementation FridaTest2
METHOD(char, char)
METHOD(int, int)
METHOD(short, short)
METHOD(long, long)
METHOD(long long, long_long)
METHOD(unsigned char, unsigned_char)
METHOD(unsigned int, unsigned_int)
METHOD(unsigned short, unsigned_short)
METHOD(unsigned long, unsigned_long)
METHOD(unsigned long long, unsigned_long_long)
METHOD(float, float)
METHOD(double, double)
METHOD(_Bool, _Bool)
METHOD(char *, char_ptr)
METHOD(id, id)
METHOD(Class, Class)
METHOD(SEL, SEL)
@end

SCRIPT_TESTCASE (method_call_preserves_value)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "var FridaTest2 = ObjC.classes.FridaTest2;"
        "function test(method, value) {"
            "var arg_value = value;"
            "if (typeof value === 'string') {"
                "arg_value = Memory.allocUtf8String(value);"
            "}"
            "var result = FridaTest2['+ _' + method + ':'](arg_value);"
            "var same = result === value;"
            "if (typeof result === 'number') {"
                "if (isNaN(result)) {"
                    "same = isNaN(value);"
                "}"
            "} else if (typeof result === 'object') {"
                "if (result instanceof NativePointer) {"
                    "same = value instanceof NativePointer &&"
                        "result.toString() === value.toString();"
                "} else if (result instanceof ObjC.Object) {"
                    "same = result.handle.toString() === value.handle.toString();"
                "}"
            "}"
            "send(same);"
        "}"
        "test('char', 127);"
        "test('char', -128);"
        "test('int', -467);"
        "test('int', 150);"
        "test('short', -56);"
        "test('short', 562);"
        "test('long',  0x7fffffff);"
        "test('long', -0x80000000);"
        "test('long_long', 0x7fffffff);"
        "test('long_long', -0x80000000);"
        "test('unsigned_char', 0);"
        "test('unsigned_char', 255);"
        "test('unsigned_int', Math.pow(2, 16) - 1);"
        "test('unsigned_int', 0x1234);"
        "test('unsigned_short', 0xffff);"
        "test('unsigned_long', 0xffffffff);"
        "test('unsigned_long_long', Math.pow(2, 63));"
        "test('float', 1.5);"
        "test('float', -5.75);"
        "test('float', -0.0);"
        "test('float', Infinity);"
        "test('float', -Infinity);"
        "test('float', NaN);"
        "test('double', Math.pow(10, 300));"
        "test('double', -Math.pow(10, 300));"
        "test('double', -0.0);"
        "test('double', Infinity);"
        "test('double', -Infinity);"
        "test('double', NaN);"
        "test('_Bool', false);"
        "test('_Bool', true);"
        "test('char_ptr', 'foobar');"
        "test('char_ptr', 'frida');"
        "test('id', FridaTest2);"
        "test('id', ObjC.classes.NSObject.new());"
        "test('Class', FridaTest2);"
        "test('Class', ObjC.classes.NSObject);"
        "test('SEL', ObjC.selector('foo'));"
        "test('SEL', ObjC.selector('foo:bar:baz:'));");

    for (gint i = 0; i != 39; i++)
    {
      EXPECT_SEND_MESSAGE_WITH ("true");
    }
  }
}

SCRIPT_TESTCASE (objects_can_be_serialized_to_json)
{
  @autoreleasepool
  {
    COMPILE_AND_LOAD_SCRIPT (
        "JSON.parse(JSON.stringify(ObjC));"
        "JSON.parse(JSON.stringify(ObjC.classes.NSObject));");
    EXPECT_NO_MESSAGES ();
  }
}

SCRIPT_TESTCASE (performance)
{
  @autoreleasepool
  {
    TestScriptMessageItem * item;
    gint duration;

    COMPILE_AND_LOAD_SCRIPT (
        "ObjC.classes.NSObject;"
        "var start = Date.now();"
        "Object.keys(ObjC.classes.NSWindow);"
        "var end = Date.now();"
        "send(end - start);");
    item = test_script_fixture_pop_message (fixture);
    sscanf (item->message, "{\"type\":\"send\",\"payload\":%d}", &duration);
    g_print ("<%d ms> ", duration);
    test_script_message_item_free (item);
  }
}
