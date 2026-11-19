# Data structures of a program's own: a linked list, a stack, a queue, a binary search tree,
# a priority queue, with the special methods that make them feel built in.
class Node:
    def __init__(self, value, next=None):
        self.value = value
        self.next = next
class LinkedList:
    def __init__(self, items=()):
        self.head = None
        self.size = 0
        for item in reversed(list(items)):
            self.push(item)
    def push(self, value):
        self.head = Node(value, self.head)
        self.size += 1
    def __iter__(self):
        node = self.head
        while node:
            yield node.value
            node = node.next
    def __len__(self):
        return self.size
    def __repr__(self):
        return " -> ".join(map(repr, self)) or "empty"
    def reverse(self):
        prev = None
        node = self.head
        while node:
            node.next, prev, node = prev, node, node.next
        self.head = prev
ll = LinkedList([1, 2, 3])
ll.push(0)
print(ll, len(ll), list(ll), 2 in ll, sum(ll))
ll.reverse()
print(ll, LinkedList())
class Stack:
    def __init__(self):
        self._items = []
    def push(self, x):
        self._items.append(x)
    def pop(self):
        if not self._items:
            raise IndexError("pop from empty stack")
        return self._items.pop()
    def __bool__(self):
        return bool(self._items)
def balanced(s):
    pairs = {")": "(", "]": "[", "}": "{"}
    st = Stack()
    for ch in s:
        if ch in "([{":
            st.push(ch)
        elif ch in pairs:
            if not st or st.pop() != pairs[ch]:
                return False
    return not st
print([balanced(s) for s in ["()", "([]{})", "(]", "((", "{[()()]}", ""]])
class Queue:
    def __init__(self):
        self.inbox, self.outbox = [], []
    def put(self, x):
        self.inbox.append(x)
    def get(self):
        if not self.outbox:
            while self.inbox:
                self.outbox.append(self.inbox.pop())
        return self.outbox.pop()
q = Queue()
for i in range(5):
    q.put(i)
print([q.get() for _ in range(3)], q.put(9), [q.get() for _ in range(3)])
class Tree:
    def __init__(self):
        self.root = None
    def insert(self, key):
        def go(node):
            if node is None:
                return [key, None, None]
            if key < node[0]:
                node[1] = go(node[1])
            elif key > node[0]:
                node[2] = go(node[2])
            return node
        self.root = go(self.root)
    def inorder(self):
        def walk(node):
            if node:
                yield from walk(node[1])
                yield node[0]
                yield from walk(node[2])
        return list(walk(self.root))
    def height(self, node="start"):
        node = self.root if node == "start" else node
        return 0 if node is None else 1 + max(self.height(node[1]), self.height(node[2]))
t = Tree()
for k in [50, 30, 70, 20, 40, 60, 80, 35, 45, 65]:
    t.insert(k)
print(t.inorder(), t.height())
class MinHeap:
    def __init__(self):
        self.data = []
    def push(self, x):
        self.data.append(x)
        i = len(self.data) - 1
        while i > 0 and self.data[(i - 1) // 2] > self.data[i]:
            p = (i - 1) // 2
            self.data[p], self.data[i] = self.data[i], self.data[p]
            i = p
    def pop(self):
        top = self.data[0]
        last = self.data.pop()
        if self.data:
            self.data[0] = last
            i = 0
            while True:
                l, r, small = 2 * i + 1, 2 * i + 2, i
                if l < len(self.data) and self.data[l] < self.data[small]:
                    small = l
                if r < len(self.data) and self.data[r] < self.data[small]:
                    small = r
                if small == i:
                    break
                self.data[i], self.data[small] = self.data[small], self.data[i]
                i = small
        return top
h = MinHeap()
for x in [5, 3, 8, 1, 9, 2, 7]:
    h.push(x)
print([h.pop() for _ in range(7)])
tasks = MinHeap()
for prio, name in [(3, "write"), (1, "plan"), (2, "code"), (1, "coffee")]:
    tasks.push((prio, name))
print([tasks.pop() for _ in range(4)])
