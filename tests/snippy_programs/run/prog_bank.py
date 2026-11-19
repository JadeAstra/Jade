# A bank: classes, inheritance, properties, exceptions of its own, class attributes, history.
class InsufficientFunds(Exception):
    def __init__(self, needed, available):
        super().__init__(f"needed {needed:.2f}, only {available:.2f} available")
        self.needed = needed
        self.available = available
class Account:
    _next_number = 1000
    interest_rate = 0.0
    def __init__(self, owner, balance=0.0):
        self.owner = owner
        self._balance = balance
        self.number = Account._next_number
        Account._next_number += 1
        self.history = []
    @property
    def balance(self):
        return self._balance
    def deposit(self, amount):
        if amount <= 0:
            raise ValueError("deposit must be positive")
        self._balance += amount
        self.history.append(("deposit", amount))
        return self
    def withdraw(self, amount):
        if amount > self._balance:
            raise InsufficientFunds(amount, self._balance)
        self._balance -= amount
        self.history.append(("withdraw", amount))
        return self
    def add_interest(self):
        interest = round(self._balance * self.interest_rate, 2)
        self._balance += interest
        return interest
    def __repr__(self):
        return f"{type(self).__name__}({self.owner!r}, {self._balance:.2f})"
    def __lt__(self, other):
        return self._balance < other._balance
class Savings(Account):
    interest_rate = 0.03
class Checking(Account):
    def __init__(self, owner, balance=0.0, overdraft=100):
        super().__init__(owner, balance)
        self.overdraft = overdraft
    def withdraw(self, amount):
        if amount > self._balance + self.overdraft:
            raise InsufficientFunds(amount, self._balance + self.overdraft)
        self._balance -= amount
        self.history.append(("withdraw", amount))
        return self
accounts = [Savings("Ann", 1000), Checking("Bob", 50), Account("Cy")]
accounts[0].deposit(500).withdraw(200)
accounts[1].withdraw(120)
accounts[2].deposit(75.5)
for a in accounts:
    print(a.number, a, a.balance, a.history)
print(sorted(accounts), max(accounts).owner, sum(a.balance for a in accounts))
print([a.add_interest() for a in accounts], accounts)
for attempt in [lambda: accounts[1].withdraw(500), lambda: accounts[2].deposit(-5), lambda: accounts[0].withdraw(10 ** 6)]:
    try:
        attempt()
    except InsufficientFunds as e:
        print("InsufficientFunds:", e, e.needed)
    except ValueError as e:
        print("ValueError:", e)
try:
    accounts[0].balance = 5
except AttributeError as e:
    print("AttributeError:", e)
print(isinstance(accounts[0], Account), Savings.interest_rate, Checking.interest_rate, Account._next_number)
totals = {}
for a in accounts:
    for kind, amount in a.history:
        totals[kind] = totals.get(kind, 0) + amount
print(totals)
