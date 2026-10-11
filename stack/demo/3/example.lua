-- 运行：lua example.lua（也可以用 luajit example.lua）
-- inner_function / outer_function 都是普通函数。
-- yield 暂停整个当前协程，包括尚未返回的普通函数调用。
-- 下一次 resume 时，从普通函数内部的 yield 后继续。

local function inner_function()
    local value = 42
    print("5.  [inner_function] yield，回到 outer_function 的 resume")
    coroutine.yield()

    print("10. [inner_function] 从 yield 后继续，局部变量 value = " .. value)
    -- 普通函数执行结束，才返回 inner 协程的函数调用处。
end

local function outer_function(inner)
    print("3.  [outer_function] resume inner")
    coroutine.resume(inner)

    print("6.  [outer_function] inner 暂停后回到这里；yield 回 main")
    coroutine.yield()

    print("9.  [outer_function] 从 yield 后继续；再次 resume inner")
    coroutine.resume(inner)

    -- inner 已结束，但对象仍然存在。outer 仍能正常 yield。
    print("12. [outer_function] inner 结束后回到这里；再次 yield 回 main")
    coroutine.yield()

    print("15. [outer_function] 从 yield 后继续；普通函数执行结束")
end

local outer = coroutine.create(function()
    local inner = coroutine.create(function()
        print("4.  [inner 协程] 调用普通函数 inner_function")
        inner_function()
        print("11. [inner 协程] inner_function 返回后到这里；内层协程结束")
    end)

    print("2.  [outer 协程] 调用普通函数 outer_function")
    outer_function(inner)
    print("16. [outer 协程] outer_function 返回后到这里；外层协程结束")
end)

print("1.  [main] 第一次 resume outer")
coroutine.resume(outer)
print("7.  [main] outer_function 内部 yield 后回到这里")

print("8.  [main] 第二次 resume outer")
coroutine.resume(outer)
print("13. [main] outer_function 内部再次 yield 后回到这里")

print("14. [main] 第三次 resume outer")
coroutine.resume(outer)
print("17. [main] outer 协程结束后回到这里；全部结束")
