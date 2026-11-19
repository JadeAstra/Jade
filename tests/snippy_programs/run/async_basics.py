# async and await: coroutines, asyncio.run, tasks interleaving at their awaits, gather, sleep,
# wait_for, queues, events, locks, async for and async with, async generators and comprehensions.
import asyncio
async def greet(name, delay):
    print("start", name)
    await asyncio.sleep(delay)
    print("end", name)
    return f"hello {name}"
async def main():
    result = await greet("solo", 0)
    print(result)
    results = await asyncio.gather(greet("a", 0.2), greet("b", 0.1), greet("c", 0))
    print(results)
    t1 = asyncio.create_task(greet("task1", 0.1))
    t2 = asyncio.create_task(greet("task2", 0))
    print("tasks made", t1.done(), t2.done())
    print(await t1, await t2, t1.done(), t1.result())
    async def ticker(name, n):
        for i in range(n):
            print(name, i)
            await asyncio.sleep(0)
        return name
    print(await asyncio.gather(ticker("x", 3), ticker("y", 2)))
    try:
        await asyncio.wait_for(asyncio.sleep(1), timeout=0.01)
    except asyncio.TimeoutError:
        print("timed out")
    print(await asyncio.wait_for(greet("quick", 0), timeout=1))
    async def fails():
        await asyncio.sleep(0)
        raise ValueError("inside a task")
    try:
        await asyncio.gather(fails(), greet("ok", 10))
    except ValueError as e:
        print("gather raised", e)
    print(await asyncio.gather(fails(), greet("kept", 0), return_exceptions=True))
    queue = asyncio.Queue()
    async def producer():
        for i in range(3):
            await queue.put(i)
            print("put", i)
            await asyncio.sleep(0)
        await queue.put(None)
    async def consumer():
        got = []
        while (item := await queue.get()) is not None:
            got.append(item)
            print("got", item)
        return got
    print(await asyncio.gather(producer(), consumer()))
    event = asyncio.Event()
    async def waiter(name):
        print(name, "waiting")
        await event.wait()
        print(name, "woke")
    waiters = [asyncio.create_task(waiter(n)) for n in ("w1", "w2")]
    await asyncio.sleep(0)
    print("setting event")
    event.set()
    await asyncio.gather(*waiters)
    lock = asyncio.Lock()
    order = []
    async def worker(n):
        async with lock:
            order.append(f"in {n}")
            await asyncio.sleep(0)
            order.append(f"out {n}")
    await asyncio.gather(worker(1), worker(2), worker(3))
    print(order)
    async def countdown(n):
        while n > 0:
            yield n
            await asyncio.sleep(0)
            n -= 1
    async for value in countdown(3):
        print("countdown", value)
    print([v async for v in countdown(4)], [v * 2 async for v in countdown(2) if v > 1])
    class Resource:
        async def __aenter__(self):
            print("opening")
            await asyncio.sleep(0)
            return self
        async def __aexit__(self, *exc):
            print("closing", exc[0])
            return False
    async with Resource() as r:
        print("using", type(r).__name__)
    task = asyncio.create_task(asyncio.sleep(10))
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        print("cancelled", task.cancelled())
    return "main done"
print(asyncio.run(main()))
async def add(a, b):
    return a + b
coro = add(1, 2)
print(type(coro).__name__, asyncio.iscoroutine(coro))
print(asyncio.run(coro))
