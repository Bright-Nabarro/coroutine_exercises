local outer = coroutine.create(function()
    local inner = coroutine.create(function()
        print("inner: 开始，然后暂停")
        coroutine.yield()

        print("inner: 继续，然后结束")
    end)

    coroutine.resume(inner) -- inner 执行到 yield，回到这里
    print("outer: 内层第一次暂停了")

    coroutine.resume(inner) -- inner 执行结束，回到这里
    print("outer: 内层状态是 " .. coroutine.status(inner)) -- dead

    print("outer: 我也要暂停，回到 main")
    coroutine.yield()

    print("outer: 我恢复了，inner 仍然是 "
        .. coroutine.status(inner)) -- dead
end)

print("main: 第一次恢复 outer")
coroutine.resume(outer)
print("main: outer 暂停，回到我这里了")

print("main: 第二次恢复 outer")
coroutine.resume(outer)
print("main: 全部结束")
