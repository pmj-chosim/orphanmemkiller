import time
from celery import Celery

app = Celery("adversarial", broker="redis://localhost:6379/0", backend="redis://localhost:6379/0")


@app.task
def ping():
    return "pong"


@app.task
def spin(seconds):
    time.sleep(seconds)
    return "done"
