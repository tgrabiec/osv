# OSv Coding Style
This document describe OSv coding style.

## 1. Indentation and Spacing
1.1 We use 4 spaces for indentation, no tabs.

1.2 switch statements, put the case with same indentation as the switch
```
    switch(op) {
    case 1:
            i++;
            break;
    case 2:
    case 3:
           i *=2;
           break;
    default:
           break;
```

1.3 Avoid multiple statements on the same line:
```
    i++; j++;
```

## 2. Breaking long lines and strings
2.1 Line length should not exceed 80 characters

## 3. Braces
3.1 Always use curly braces for if statement, even if it is a one line if.

3.2 In inline method, you can use the open braces at the same line of the method.
```
    int get_age() {
        return age;
    }
```

3.3 In longer method,  the opening brace should be at the beginning of the line.
```
    void clear()
    {
       .....
    }
```

## 4. Naming Convention
4.1 Use all lower snake_case names

## 5. Commenting
5.1 Use the // C++ comment style for normal comment
5.2 When documenting a namespace, class, method or function using Doxygen, use /** */ comments.

## 6. Macros, Enums and RTL
6.1 Avoid Macros when a method would do. Prefer enum and constant to macro.
6.2 Prefer "enum class" to "enum".

6.3 Macro names and enum label should be capitalized. For "enum class",
non-capitalized values are fine.
