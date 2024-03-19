# Chity Chat
> Chity Chat is a simple Chat App with the purpose of learning. It includes a website code and a custom web server written in C.

### Chat App features
* **Multiple Groups:** Users can join any group; everything is public.
* **Real-time Communication:** Messages are sent and received instantly.
* **User Profile Pictures:** Users can upload and change their profile pictures.

### Planned features
- [ ] **Private Groups:** Invitation-only groups.
   - [ ] Mark group as private, only group members can get it.
   - [ ] Create invites/codes to join.
      - [ ] Front-end implementation: Create links/codes, join with code, and links.
         - [ ] Settings on codes/links like expire date and max uses.
      - [ ] Back-end implementation:
         - [ ] Code management
         - [ ] Link management (e.g. https://localhost:8080/invites/6141611) - how would I implement this? 
- [ ] **Group Member Roles:** Admins, mods, etc.
- [ ] **User Account Management:** Change username, display name, bio, and password.
- [ ] **Deletion:** Ability to delete accounts, messages, and groups.
- [ ] **Direct Messaging (DM):** Private messaging between users.
- [ ] **Enhanced Messaging:** Send photos, videos, files, reply to messages, and edit messages.
   - [ ] Front-end implementation
      - [ ] Add images and paste image from clipboard.
      - [ ] Reply button on messages 
   - [ ] Back-end implementation 
- [ ] **Mobile Compatibility:** Improve usability on mobile devices.
- [ ] **URL Parameters Support:** Support HTTP URL parameters.
- [ ] **Real-Time User Status Management:** Online, Offline, Away, busy, typing, etc.
- [x] **Upload File Management:** Avoid duplication user files.
   - [ ] Default profile pic
- [ ] **Real-Time User Profile Updates:** Receive instant updates for user profile changes like usernames, display names, bio, and profile pictures.

### Web server features
* **HTTP/1.1 Parsing:** Basic parsing with support for GET and POST requests.
* **Web Sockets Implementation:** Real-time communication support.
* **SSL/TLS via OpenSSL:** Secure connections.
* **Password Security:** Salting and hashing using SHA512.
* **I/O Multiplexing:** Utilizes [epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html) for efficient I/O.
* **Database Management:** Uses SQLite3 for SQL database.
* **Session Management:** Users receive session IDs for persistent login.

### Learning experience
> Through building Chity Chat, I learned:
* Web Server Implementation:
    * HTTP
    * Web Sockets
    * SSL/TLS
* SQL Database Usage with SQLite3.
* Frontend Development with JavaScript, HTML, and CSS.
* Password Security with SHA512 hashing.
