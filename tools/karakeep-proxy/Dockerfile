# syntax=docker/dockerfile:1
# Ultra-minimal image using Alpine Linux + pre-compiled musl wheels.
FROM python:3.12-alpine

WORKDIR /app

# libxml2 and libxslt runtime libs are needed by lxml wheels even on Alpine
RUN apk add --no-cache libxml2 libxslt

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY main.py .

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

EXPOSE 8787

ENTRYPOINT ["python3", "main.py"]
