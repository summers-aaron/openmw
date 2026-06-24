#ifndef OPENMW_MWNULL_NULLSOUNDMANAGER_H
#define OPENMW_MWNULL_NULLSOUNDMANAGER_H

#include "../mwbase/soundmanager.hpp"

namespace MWNull
{
    /// \brief No-op SoundManager implementation for headless/dedicated-server builds.
    class NullSoundManager : public MWBase::SoundManager
    {
    public:
        NullSoundManager() = default;
        ~NullSoundManager() override = default;

        void update(float duration) override {}

        void processChangedSettings(const std::set<std::pair<std::string, std::string>>& settings) override {}

        bool isEnabled() const override { return false; }

        void stopMusic() override {}

        MWSound::MusicType getMusicType() const override { return MWSound::MusicType::Normal; }

        void streamMusic(VFS::Path::NormalizedView filename, MWSound::MusicType type, float fade) override {}

        bool isMusicPlaying() override { return false; }

        void say(const MWWorld::ConstPtr& reference, VFS::Path::NormalizedView filename) override {}

        void say(VFS::Path::NormalizedView filename) override {}

        bool sayActive(const MWWorld::ConstPtr& reference) const override { return false; }

        bool sayDone(const MWWorld::ConstPtr& reference) const override { return false; }

        void stopSay(const MWWorld::ConstPtr& reference) override {}

        float getSaySoundLoudness(const MWWorld::ConstPtr& reference) const override { return 0; }

        MWBase::SoundStream* playTrack(const MWSound::DecoderPtr& decoder, Type type) override { return nullptr; }

        void stopTrack(MWBase::SoundStream* stream) override {}

        double getTrackTimeDelay(MWBase::SoundStream* stream) override { return 0; }

        MWBase::Sound* playSound(const ESM::RefId& soundId, float volume, float pitch, Type type, PlayMode mode, float offset)
            override
        {
            return nullptr;
        }

        MWBase::Sound* playSound(VFS::Path::NormalizedView fileName, float volume, float pitch, Type type, PlayMode mode,
            float offset) override
        {
            return nullptr;
        }

        MWBase::Sound* playSound3D(const MWWorld::ConstPtr& reference, const ESM::RefId& soundId, float volume, float pitch,
            Type type, PlayMode mode, float offset) override
        {
            return nullptr;
        }

        MWBase::Sound* playSound3D(const MWWorld::ConstPtr& reference, VFS::Path::NormalizedView fileName, float volume,
            float pitch, Type type, PlayMode mode, float offset) override
        {
            return nullptr;
        }

        MWBase::Sound* playSound3D(const osg::Vec3f& initialPos, const ESM::RefId& soundId, float volume, float pitch, Type type,
            PlayMode mode, float offset) override
        {
            return nullptr;
        }

        void stopSound(MWBase::Sound* sound) override {}

        void stopSound3D(const MWWorld::ConstPtr& reference, const ESM::RefId& soundId) override {}

        void stopSound3D(const MWWorld::ConstPtr& reference, VFS::Path::NormalizedView fileName) override {}

        void stopSound3D(const MWWorld::ConstPtr& reference) override {}

        void stopSound(const MWWorld::CellStore* cell) override {}

        void fadeOutSound3D(const MWWorld::ConstPtr& reference, const ESM::RefId& soundId, float duration) override {}

        bool getSoundPlaying(const MWWorld::ConstPtr& reference, const ESM::RefId& soundId) const override
        {
            return false;
        }

        bool getSoundPlaying(const MWWorld::ConstPtr& reference, VFS::Path::NormalizedView fileName) const override
        {
            return false;
        }

        void pauseSounds(MWSound::BlockerType blocker, int types) override {}

        void resumeSounds(MWSound::BlockerType blocker) override {}

        void pausePlayback() override {}

        void resumePlayback() override {}

        void setListenerPosDir(
            const osg::Vec3f& pos, const osg::Vec3f& dir, const osg::Vec3f& up, bool underwater) override
        {
        }

        void setListenerVel(const osg::Vec3f& vel) override {}

        void updatePtr(const MWWorld::ConstPtr& old, const MWWorld::ConstPtr& updated) override {}

        void clear() override {}
    };
}

#endif
