^title Num Class
^category core

## Static Methods

### Num.**fromString**(value)

Attempts to parse `value` as a decimal literal and return it as an instance of
`Num`. If the number cannot be parsed `null` will be returned.

It is a runtime error if `value` is not a string.

### Num.**pi**

The value of π.

## Methods

### **abs**

The absolute value of the number.

    :::dart
    -123.abs // 123

### **acos**

The arc cosine of the number.

### **asin**

The arc sine of the number.

### **atan**

The arc tangent of the number.

### **atan**(x)

The arc tangent of the number when divided by `x`, using the signs of the two
numbers to determine the quadrant of the result.

### **ceil**

Rounds the number up to the nearest integer.

    :::dart
    1.5.ceil // 2
    (-3.2).ceil // -3

### **cos**

The cosine of the number.

### **floor**

Rounds the number down to the nearest integer.

    :::dart
    1.5.floor    // 1
    (-3.2).floor // -4

### **isNan**

Whether the number is [not a number](http://en.wikipedia.org/wiki/NaN). This is
`false` for normal number values and infinities, and `true` for the result of
`0/0`, the square root of a negative number, etc.

### **sin**

The sine of the number.

### **sqrt**

The square root of the number. Returns `nan` if the number is negative.

### **tan**

The tangent of the number.

### **-** operator

Negates the number.

    :::dart
    var a = 123
    -a // -123

### **-**(other), **+**(other), **/**(other), **\***(other) operators

The usual arithmetic operators you know and love. All of them do 64-bit
floating point arithmetic. It is a runtime error if the right-hand operand is
not a number. Wren doesn't roll with implicit conversions.

### **%**(denominator) operator

The floating-point remainder of this number divided by `denominator`.

It is a runtime error if `denominator` is not a number.

### **&lt;**(other), **&gt;**(other), **&lt;=**(other), **&gt;=**(other) operators

Compares this and `other`, returning `true` or `false` based on how the numbers
are ordered. It is a runtime error if `other` is not a number.

### **~** operator

Performs *bitwise* negation on the number. The number is first converted to a
32-bit unsigned value, which will truncate any floating point value. The bits
of the result of that are then negated, yielding the result.

### **&**(other) operator

Performs bitwise and on the number. Both numbers are first converted to 32-bit
unsigned values. The result is then a 32-bit unsigned number where each bit is
`true` only where the corresponding bits of both inputs were `true`.

It is a runtime error if `other` is not a number.

### **|**(other) operator

Performs bitwise or on the number. Both numbers are first converted to 32-bit
unsigned values. The result is then a 32-bit unsigned number where each bit is
`true` only where the corresponding bits of both inputs were `true`.

It is a runtime error if `other` is not a number.

### **..**(other) operator

Creates a [Range](core/range.html) representing a consecutive range of numbers
from the beginning number to the ending number.

    :::dart
    var range = 1.2..3.4
    IO.print(range.min)         // 1.2
    IO.print(range.max)         // 3.4
    IO.print(range.isInclusive) // true

### **...**(other) operator

Creates a [Range](core/range.html) representing a consecutive range of numbers
from the beginning number to the ending number not including the ending number.

    :::dart
    var range = 1.2...3.4
    IO.print(range.min)         // 1.2
    IO.print(range.max)         // 3.4
    IO.print(range.isInclusive) // false
