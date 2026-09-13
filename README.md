# TubeLite — C++ video-sharing platform

A YouTube-style video-sharing website built with a C++ backend. It deliberately does **not** copy YouTube's branding, proprietary UI, assets, or source code.

## Included

- C++17 backend
- Registration and login
- Session cookies
- Video upload with title, description and thumbnail URL
- Video playback
- Home feed and search
- Likes
- Comments
- Creator/channel pages
- View counter
- Responsive dark UI
- Docker + Render configuration
- SQLite metadata database
- Local video storage
- Basic MIME/type and upload-size checks

## Important Render limitation

The included app stores uploaded videos and SQLite data under `/app/data`. Render's free web service filesystem is ephemeral, so uploads can disappear after a restart/redeploy. This is a hosting limitation, not a bug in the app.

For a real public service, replace the local `VideoStore` layer with object storage (S3-compatible storage, Cloudinary, etc.) and use a persistent database.

## Run locally

```bash
docker build -t tubelite .
docker run --rm -p 10000:10000 -v "$(pwd)/data:/app/data" tubelite
```

Open:

http://localhost:10000

## Render

1. Put this project in a GitHub repository.
2. Create a new Render Web Service from the repo.
3. Choose Docker.
4. The included `render.yaml` can also be used with a Blueprint.
5. Deploy.

No API key is required for the basic local-storage version.

## Upload policy

The demo intentionally limits upload size to 512 MB. In production, use direct-to-object-storage uploads, transcoding, moderation, rate limiting, CSRF protection, stronger session storage, virus scanning, and abuse reporting.
